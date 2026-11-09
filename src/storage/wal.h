#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "storage/buffer.h"

namespace mvd {

enum WalType : uint32_t {
  kWalRedoPage = 1,
  kWalCommit = 2,
  kWalCheckpoint = 3,
};

struct WalRecordHeader {
  uint64_t lsn = 0;
  uint32_t xid = 0;
  uint32_t type = 0;
  uint32_t len = 0;   // payload length
  uint32_t crc = 0;
};

// Redo-only write-ahead log holding whole-page images.
//
// Nothing is written to the log until a transaction commits, so a torn tail
// can only contain records of transactions that never reached a COMMIT
// record; recovery ignores those, which is exactly the "提交请求未收到成功
// 响应则完整不生效" rule in the task statement.
class Wal {
 public:
  Wal() = default;

  void open(const std::string& path);
  void close();
  bool open_() const { return fd_ >= 0; }
  const std::string& path() const { return path_; }

  // Appends a redo image. Caller must follow with appendCommit + sync.
  void appendRedo(uint32_t oid, uint32_t page, const uint8_t* data, size_t n);
  void appendCommit(uint32_t xid);
  void appendCheckpoint(uint64_t base_lsn, uint64_t next_lsn);
  void sync();

  uint64_t nextLsn() const { return next_lsn_; }
  uint64_t baseLsn() const { return base_lsn_; }
  size_t sizeBytes() const;

  // Drops the log once every record it holds is reflected in the data files.
  void resetTo(uint64_t base_lsn, uint64_t next_lsn);

  struct ScanResult {
    uint64_t base_lsn = 0;
    uint64_t next_lsn = 0;
    bool truncated = false;  // a torn record ended the scan early
    std::unordered_set<uint32_t> committed;
    uint64_t redo_records = 0;
  };
  static ScanResult scan(const std::string& path, size_t page_size);

  // Re-applies committed redo images straight into the data files. `resolve`
  // maps a table oid to the open descriptor of its heap file. Returns the
  // number of pages restored.
  size_t applyRedo(const std::string& path, size_t page_size,
                   const std::unordered_set<uint32_t>& committed, uint64_t base_lsn,
                   const std::function<bool(uint32_t oid, int& fd)>& resolve);

 private:
  void writeRecord(const WalRecordHeader& h, const uint8_t* payload, size_t n);

  std::string path_;
  int fd_ = -1;
  uint64_t next_lsn_ = 1;
  uint64_t base_lsn_ = 0;
  // Redo images of the transaction currently committing. Nothing reaches the
  // log before COMMIT, which is what makes recovery of an aborted or crashed
  // transaction a no-op.
  std::vector<std::vector<uint8_t>> pending_;
};

}  // namespace mvd
