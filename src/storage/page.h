#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace mvd {

// Slotted page layout.
//
//   [PageHeader][slot0 slot1 ...][ .... free .... ][rec_n ... rec_1 rec_0]
//
// Slots grow up from the header, record bytes grow down from the page end, so
// a page never needs to move existing records.
struct PageHeader {
  uint16_t type = 0;
  uint16_t slot_count = 0;
  uint16_t free_off = 0;   // first byte after the slot array
  uint16_t used_end = 0;   // lowest byte occupied by record data
  uint16_t checksum = 0;
  uint16_t flags = 0;
};

struct Slot {
  uint16_t off = 0;
  uint16_t len = 0;
  uint16_t flags = 0;  // 0 = deleted/empty, 1 = live
  uint16_t pad = 0;
};

static constexpr uint16_t kSlotLive = 1;
static constexpr uint16_t kPageHeap = 1;
static constexpr uint16_t kPageHeaderFile = 2;

inline PageHeader& headerOf(uint8_t* page) { return *reinterpret_cast<PageHeader*>(page); }
inline const PageHeader& headerOf(const uint8_t* page) {
  return *reinterpret_cast<const PageHeader*>(page);
}
inline Slot& slotAt(uint8_t* page, uint16_t i) {
  return *reinterpret_cast<Slot*>(page + sizeof(PageHeader) + static_cast<size_t>(i) * sizeof(Slot));
}
inline const Slot& slotAt(const uint8_t* page, uint16_t i) {
  return *reinterpret_cast<const Slot*>(page + sizeof(PageHeader) +
                                        static_cast<size_t>(i) * sizeof(Slot));
}

inline uint16_t pageChecksum(const uint8_t* page, size_t n) {
  // FNV-1a over the page with the checksum field zeroed. Cheap and enough to
  // reject torn or misdirected pages; it is not a cryptographic digest.
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = (i >= offsetof(PageHeader, checksum) && i < offsetof(PageHeader, checksum) + 2)
                    ? 0
                    : page[i];
    h ^= b;
    h *= 16777619u;
  }
  return static_cast<uint16_t>(h ^ (h >> 16));
}

inline void initPage(uint8_t* page, size_t page_size, uint16_t type) {
  std::memset(page, 0, page_size);
  PageHeader& h = headerOf(page);
  h.type = type;
  h.slot_count = 0;
  h.free_off = static_cast<uint16_t>(sizeof(PageHeader));
  h.used_end = static_cast<uint16_t>(page_size);
  h.checksum = pageChecksum(page, page_size);
}

inline uint16_t pageFreeSpace(const uint8_t* page) {
  const PageHeader& h = headerOf(page);
  return h.used_end > h.free_off ? static_cast<uint16_t>(h.used_end - h.free_off) : 0;
}

// Appends a record image into the page. Returns the slot index, or -1 when the
// page cannot hold it (record plus 8-byte slot does not fit the free region).
// Record starts are kept 8-byte aligned so the vector payload can feed SIMD
// kernels directly from the page.
inline int pageInsertRecord(uint8_t* page, size_t page_size, const uint8_t* data, uint16_t len) {
  PageHeader& h = headerOf(page);
  const uint16_t aligned = static_cast<uint16_t>((len + 7u) & ~7u);
  size_t need = static_cast<size_t>(aligned) + sizeof(Slot);
  if (need > pageFreeSpace(page)) return -1;
  uint16_t idx = h.slot_count++;
  h.used_end = static_cast<uint16_t>(h.used_end - aligned);
  h.free_off = static_cast<uint16_t>(h.free_off + sizeof(Slot));
  Slot& s = slotAt(page, idx);
  s.off = h.used_end;
  s.len = len;
  s.flags = kSlotLive;
  std::memcpy(page + s.off, data, len);
  (void)page_size;
  h.checksum = pageChecksum(page, page_size);
  return idx;
}

// Marks a slot empty, reclaiming nothing: the record bytes stay until the page
// is rewritten by compaction or checkpoint.
inline bool pageDeleteRecord(uint8_t* page, size_t page_size, uint16_t idx) {
  PageHeader& h = headerOf(page);
  if (idx >= h.slot_count) return false;
  Slot& s = slotAt(page, idx);
  if (!(s.flags & kSlotLive)) return false;
  s.flags = 0;
  h.checksum = pageChecksum(page, page_size);
  return true;
}

// Overwrites a live slot in place. Only valid when the new image is not longer
// than the old one; the caller pads with zeros.
inline bool pageUpdateRecord(uint8_t* page, size_t page_size, uint16_t idx, const uint8_t* data,
                            uint16_t len) {
  PageHeader& h = headerOf(page);
  if (idx >= h.slot_count) return false;
  Slot& s = slotAt(page, idx);
  if (!(s.flags & kSlotLive) || len > s.len) return false;
  std::memcpy(page + s.off, data, len);
  if (len < s.len) std::memset(page + s.off + len, 0, s.len - len);
  h.checksum = pageChecksum(page, page_size);
  return true;
}

// ---------------------------------------------------------------------------
// Record image: [uint16 ncols][uint16 pad][uint32 offs[ncols+1]][payload]
//
// The offset table is 4-byte wide so every value start is 4-byte aligned
// relative to an 8-byte aligned record start, which keeps float32 payloads
// directly usable by SIMD kernels.
// ---------------------------------------------------------------------------

static constexpr size_t kRecordPrefix = 4;

inline size_t recordValueSize(const Value& v, const Column& c) {
  switch (c.type) {
    case DataType::Bigint: return 8;
    case DataType::Varchar: return v.s.size();
    case DataType::Vector: return static_cast<size_t>(c.vector_dim) * 4;
    case DataType::Real: break;  // never a column type
  }
  return 0;
}

inline size_t recordEncodedSize(const Row& row, const std::vector<Column>& cols) {
  size_t n = kRecordPrefix + sizeof(uint32_t) * (cols.size() + 1);
  for (size_t i = 0; i < cols.size(); ++i) n += recordValueSize(row[i], cols[i]);
  return n;
}

inline void encodeRecord(const Row& row, const std::vector<Column>& cols, uint8_t* dst,
                         size_t total) {
  const uint16_t nc = static_cast<uint16_t>(cols.size());
  std::memcpy(dst, &nc, 2);
  std::memset(dst + 2, 0, 2);
  uint32_t* offs = reinterpret_cast<uint32_t*>(dst + kRecordPrefix);
  uint32_t off = static_cast<uint32_t>(kRecordPrefix + sizeof(uint32_t) * (nc + 1));
  offs[0] = off;
  for (uint32_t i = 0; i < nc; ++i) {
    off += static_cast<uint32_t>(recordValueSize(row[i], cols[i]));
    offs[i + 1] = off;
  }
  for (uint32_t i = 0; i < nc; ++i) {
    uint8_t* p = dst + offs[i];
    switch (cols[i].type) {
      case DataType::Bigint: {
        int64_t x = row[i].i;
        std::memcpy(p, &x, 8);
        break;
      }
      case DataType::Varchar:
        std::memcpy(p, row[i].s.data(), row[i].s.size());
        break;
      case DataType::Vector:
        std::memcpy(p, row[i].v.data(), static_cast<size_t>(cols[i].vector_dim) * 4);
        break;
      case DataType::Real:
        break;  // never a column type
    }
  }
  (void)total;
}

inline void decodeRecord(const uint8_t* src, const std::vector<Column>& cols, Row& out) {
  uint16_t nc = 0;
  std::memcpy(&nc, src, 2);
  const uint32_t* offs = reinterpret_cast<const uint32_t*>(src + kRecordPrefix);
  out.clear();
  out.resize(nc);
  for (uint16_t i = 0; i < nc; ++i) {
    const uint32_t o = offs[i], o2 = offs[i + 1];
    const uint8_t* p = src + o;
    switch (cols[i].type) {
      case DataType::Bigint:
        std::memcpy(&out[i].i, p, 8);
        out[i].type = DataType::Bigint;
        break;
      case DataType::Varchar:
        out[i].type = DataType::Varchar;
        out[i].s.assign(reinterpret_cast<const char*>(p), o2 - o);
        break;
      case DataType::Vector: {
        out[i].type = DataType::Vector;
        size_t dim = (o2 - o) / 4;
        out[i].v.resize(dim);
        std::memcpy(out[i].v.data(), p, dim * 4);
        break;
      }
      case DataType::Real:
        break;  // never a column type
    }
  }
}

// Direct accessors used by the scan kernels so a TopK query never materialises
// a full Row.
inline int64_t recordBigint(const uint8_t* src, size_t col) {
  const uint32_t* offs = reinterpret_cast<const uint32_t*>(src + kRecordPrefix);
  int64_t v;
  std::memcpy(&v, src + offs[col], 8);
  return v;
}
inline const float* recordVector(const uint8_t* src, size_t col) {
  const uint32_t* offs = reinterpret_cast<const uint32_t*>(src + kRecordPrefix);
  return reinterpret_cast<const float*>(src + offs[col]);
}
inline std::string_view recordVarchar(const uint8_t* src, size_t col) {
  const uint32_t* offs = reinterpret_cast<const uint32_t*>(src + kRecordPrefix);
  return std::string_view(reinterpret_cast<const char*>(src + offs[col]),
                          offs[col + 1] - offs[col]);
}

}  // namespace mvd
