#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/errors.h"

namespace mvd {

struct PageRef {
  uint32_t file = 0;
  uint32_t page = 0;
  bool operator==(const PageRef& o) const { return file == o.file && page == o.page; }
};

struct PageRefHash {
  size_t operator()(const PageRef& r) const {
    return (static_cast<size_t>(r.file) << 32) ^ r.page;
  }
};

// A resident page frame. `data` is aligned to at least 512 bytes so O_DIRECT
// transfers are legal without an extra copy.
struct Frame {
  PageRef ref;
  std::vector<uint8_t> buf;  // owns page_size + alignment slack
  uint8_t* data = nullptr;
  size_t slot = 0;
  bool resident = false;
  bool dirty = false;
  bool fresh = false;  // allocated but never written to disk yet
  uint32_t xid = 0;
  int pin = 0;
  int clock = 0;
};

class BufferPool {
 public:
  BufferPool(size_t page_size, size_t max_frames);

  uint32_t openFile(const std::string& path, bool create, bool direct);
  uint32_t createFile(const std::string& path);
  void closeFile(uint32_t fid);
  int fileFd(uint32_t fid) const { return files_[fid].fd; }
  // Whether the data files really opened with O_DIRECT. The benchmark counts
  // page cache in memory.peak, so a silent fallback changes what the numbers
  // mean and has to be visible.
  size_t directFileCount() const {
    size_t n = 0;
    for (size_t i = 1; i < files_.size(); ++i)
      if (files_[i].direct) ++n;
    return n;
  }

  size_t pageSize() const { return page_size_; }
  size_t frames() const { return pool_.size(); }

  // Fetches a page and pins it. The caller must release the guard before
  // fetching an unbounded number of other pages.
  Frame* fetch(uint32_t fid, uint32_t pageNo);
  Frame* fetchNew(uint32_t fid, uint32_t pageNo);
  void unpin(Frame* f);

  uint64_t fileSizePages(uint32_t fid) const;
  void truncate(uint32_t fid, uint64_t pages);

  void markDirty(Frame* f);
  void flush(Frame* f);
  void flushFile(uint32_t fid);
  void flushAll();

  // Pages dirtied by the not-yet-committed transaction must never reach the
  // data file, because the WAL redo image is only written at commit.
  void setActiveXid(uint32_t xid) { active_xid_ = xid; }
  uint32_t activeXid() const { return active_xid_; }

  // Drops every clean resident page of a file (used when a file is deleted).
  void forgetFile(uint32_t fid);

  uint64_t reads() const { return reads_; }
  uint64_t writes() const { return writes_; }

 private:
  Frame* findResident(const PageRef& r) const;
  Frame* pickVictim();
  void readInto(Frame& f);
  void writeOut(Frame& f);

  size_t page_size_;
  size_t max_frames_;
  uint32_t active_xid_ = 0;

  struct FileEntry {
    int fd = -1;
    std::string path;
    bool direct = false;
  };
  std::vector<FileEntry> files_;  // index 0 unused; file ids start at 1

  std::vector<std::unique_ptr<Frame>> pool_;
  std::unordered_map<PageRef, size_t, PageRefHash> index_;
  size_t clock_hand_ = 0;
  uint64_t reads_ = 0;
  uint64_t writes_ = 0;
};

// RAII pin.
class PageGuard {
 public:
  PageGuard() = default;
  PageGuard(BufferPool* p, Frame* f) : pool_(p), frame_(f) {}
  PageGuard(PageGuard&& o) noexcept : pool_(o.pool_), frame_(o.frame_) { o.frame_ = nullptr; }
  PageGuard& operator=(PageGuard&& o) noexcept {
    if (this != &o) {
      reset();
      pool_ = o.pool_;
      frame_ = o.frame_;
      o.frame_ = nullptr;
    }
    return *this;
  }
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  ~PageGuard() { reset(); }

  void reset() {
    if (frame_) pool_->unpin(frame_);
    frame_ = nullptr;
  }
  uint8_t* data() const { return frame_ ? frame_->data : nullptr; }
  Frame* frame() const { return frame_; }
  explicit operator bool() const { return frame_ != nullptr; }

 private:
  BufferPool* pool_ = nullptr;
  Frame* frame_ = nullptr;
};

}  // namespace mvd
