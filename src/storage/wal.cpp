#include "storage/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mvd {

static uint64_t fnv1a64(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static bool readFull(int fd, uint8_t* dst, size_t n, size_t* got) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, dst + off, n - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) {
      *got = off;
      return off == n;
    }
    off += static_cast<size_t>(r);
  }
  *got = off;
  return true;
}

void Wal::open(const std::string& path) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) throw std::runtime_error("open wal: " + std::string(std::strerror(errno)));
  struct stat st;
  ::fstat(fd_, &st);
  if (st.st_size == 0) {
    next_lsn_ = 1;
    base_lsn_ = 0;
  }
}

void Wal::close() {
  if (fd_ >= 0) {
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

size_t Wal::sizeBytes() const {
  struct stat st;
  if (fd_ < 0 || ::fstat(fd_, &st) != 0) return 0;
  return static_cast<size_t>(st.st_size);
}

void Wal::writeRecord(const WalRecordHeader& h, const uint8_t* payload, size_t n) {
  WalRecordHeader out = h;
  out.lsn = next_lsn_++;
  out.len = static_cast<uint32_t>(n);
  out.crc = n ? static_cast<uint32_t>(fnv1a64(payload, n)) : 0;
  // The header is 32 bytes, so records stay naturally aligned for later
  // sequential reads; the page image inside a redo payload inherits the
  // caller's 512-byte aligned buffer.
  uint8_t buf[sizeof(WalRecordHeader)];
  std::memcpy(buf, &out, sizeof(out));
  size_t off = 0;
  while (off < sizeof(buf)) {
    ssize_t w = ::write(fd_, buf + off, sizeof(buf) - off);
    if (w <= 0) throw std::runtime_error("wal write header");
    off += static_cast<size_t>(w);
  }
  off = 0;
  while (off < n) {
    ssize_t w = ::write(fd_, payload + off, n - off);
    if (w <= 0) throw std::runtime_error("wal write payload");
    off += static_cast<size_t>(w);
  }
}

void Wal::appendRedo(uint32_t oid, uint32_t page, const uint8_t* data, size_t n) {
  std::vector<uint8_t> payload(8 + n);
  std::memcpy(payload.data(), &oid, 4);
  std::memcpy(payload.data() + 4, &page, 4);
  std::memcpy(payload.data() + 8, data, n);
  pending_.push_back(std::move(payload));
}

void Wal::appendCommit(uint32_t xid) {
  // Flush the buffered redo images first, then the COMMIT record, then one
  // fsync: a single durability point per transaction.
  for (auto& p : pending_) {
    WalRecordHeader h;
    h.xid = xid;
    h.type = kWalRedoPage;
    writeRecord(h, p.data(), p.size());
  }
  pending_.clear();
  WalRecordHeader h;
  h.xid = xid;
  h.type = kWalCommit;
  writeRecord(h, nullptr, 0);
}

void Wal::appendCheckpoint(uint64_t base_lsn, uint64_t next_lsn) {
  uint8_t payload[16];
  std::memcpy(payload, &base_lsn, 8);
  std::memcpy(payload + 8, &next_lsn, 8);
  WalRecordHeader h;
  h.type = kWalCheckpoint;
  writeRecord(h, payload, sizeof(payload));
  base_lsn_ = base_lsn;
}

void Wal::sync() {
  if (fd_ >= 0 && ::fsync(fd_) != 0)
    throw std::runtime_error("wal fsync: " + std::string(std::strerror(errno)));
}

void Wal::resetTo(uint64_t base, uint64_t next) {
  if (fd_ < 0) return;
  if (::ftruncate(fd_, 0) != 0) throw std::runtime_error("wal truncate");
  if (::lseek(fd_, 0, SEEK_SET) != 0) throw std::runtime_error("wal seek");
  base_lsn_ = base;
  // LSN 0 is reserved for "nothing", because recovery skips records at or
  // below the base LSN; a first record numbered 0 would be unrecoverable.
  next_lsn_ = next < 1 ? 1 : next;
  sync();
}

Wal::ScanResult Wal::scan(const std::string& path, size_t page_size) {
  ScanResult r;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return r;
  std::vector<uint8_t> hdr(sizeof(WalRecordHeader));
  std::vector<uint8_t> payload;
  while (true) {
    size_t got = 0;
    if (!readFull(fd, hdr.data(), hdr.size(), &got) || got != hdr.size()) break;
    WalRecordHeader h;
    std::memcpy(&h, hdr.data(), sizeof(h));
    if (h.len > 64u * 1024u * 1024u) break;  // implausible: treat as torn
    payload.resize(h.len);
    // A COMMIT record carries no payload; skipping the body read for it is
    // what previously left a stale byte count here and truncated the scan.
    if (h.len != 0) {
      got = 0;
      if (!readFull(fd, payload.data(), h.len, &got) || got != h.len) break;
    }
    uint32_t crc = h.len ? static_cast<uint32_t>(fnv1a64(payload.data(), h.len)) : 0;
    if (crc != h.crc) break;  // torn body: everything after is unusable
    r.next_lsn = h.lsn + 1;
    switch (h.type) {
      case kWalCommit:
        r.committed.insert(h.xid);
        break;
      case kWalCheckpoint: {
        if (h.len >= 16) {
          std::memcpy(&r.base_lsn, payload.data(), 8);
          std::memcpy(&r.next_lsn, payload.data() + 8, 8);
        }
        break;
      }
      case kWalRedoPage:
        if (h.len >= 8 + page_size) r.redo_records += 1;
        break;
      default:
        break;
    }
  }
  struct stat st;
  if (::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) != 0) {
    off_t pos = ::lseek(fd, 0, SEEK_CUR);
    if (pos < static_cast<off_t>(st.st_size)) r.truncated = true;
  }
  ::close(fd);
  return r;
}

size_t Wal::applyRedo(const std::string& path, size_t page_size,
                      const std::unordered_set<uint32_t>& committed, uint64_t base_lsn,
                      const std::function<bool(uint32_t oid, int& fd)>& resolve) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return 0;
  std::vector<uint8_t> hdr(sizeof(WalRecordHeader));
  std::vector<uint8_t> payload;
  size_t applied = 0;
  while (true) {
    size_t got = 0;
    if (!readFull(fd, hdr.data(), hdr.size(), &got) || got != hdr.size()) break;
    WalRecordHeader h;
    std::memcpy(&h, hdr.data(), sizeof(h));
    payload.resize(h.len);
    if (h.len && (!readFull(fd, payload.data(), h.len, &got) || got != h.len)) break;
    if (h.len && static_cast<uint32_t>(fnv1a64(payload.data(), h.len)) != h.crc) break;
    if (h.type != kWalRedoPage || h.lsn <= base_lsn) continue;
    if (!committed.count(h.xid)) continue;
    if (h.len < 8 + page_size) continue;
    uint32_t oid = 0, page = 0;
    std::memcpy(&oid, payload.data(), 4);
    std::memcpy(&page, payload.data() + 4, 4);
    int data_fd = -1;
    if (!resolve(oid, data_fd)) continue;  // table dropped after the log entry
    ssize_t w = ::pwrite(data_fd, payload.data() + 8, page_size,
                         static_cast<off_t>(page) * page_size);
    if (w != static_cast<ssize_t>(page_size)) {
      ::close(fd);
      throw std::runtime_error("wal redo pwrite failed");
    }
    applied += 1;
  }
  ::close(fd);
  return applied;
}

}  // namespace mvd
