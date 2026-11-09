#include "engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/fs.h"

namespace mvd {

static std::string sidecarPath(const Config* cfg, uint32_t oid, const char* suffix) {
  return joinPath(cfg->data_dir, "t" + std::to_string(oid) + suffix);
}

// ---------------------------------------------------------------------------
// HeapHeader
// ---------------------------------------------------------------------------

void HeapHeader::computeCrc() {
  HeapHeader copy = *this;
  copy.crc = 0;
  uint32_t h = 2166136261u;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&copy);
  for (size_t i = 0; i < sizeof(copy); ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  crc = h;
}

bool HeapHeader::checkCrc() const {
  HeapHeader copy = *this;
  copy.computeCrc();
  return copy.crc == crc;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

std::string Table::heapPath() const {
  return joinPath(cfg_->data_dir, "t" + std::to_string(oid_) + ".heap");
}

void Table::openFiles(bool create) {
  heap_fid_ = pool_->openFile(heapPath(), create, cfg_->direct_io);
}

void Table::closeFiles() {
  if (heap_fid_) pool_->closeFile(heap_fid_);
  heap_fid_ = 0;
}

void Table::dropFiles() {
  if (heap_fid_) {
    pool_->forgetFile(heap_fid_);
    pool_->closeFile(heap_fid_);
    heap_fid_ = 0;
  }
  ::unlink(heapPath().c_str());
  ::unlink(sidecarPath(cfg_, oid_, ".vidx").c_str());
  ::unlink(sidecarPath(cfg_, oid_, ".pki").c_str());
}

void Table::fsyncData() {
  if (heap_fid_) ::fsync(pool_->fileFd(heap_fid_));
}

void Table::readHeader() {
  PageGuard g(pool_, pool_->fetch(heap_fid_, 0));
  HeapHeader h;
  std::memcpy(&h, g.data(), sizeof(h));
  if (h.magic == 0x4D564831u && h.checkCrc()) {
    header_ = h;
  } else {
    header_ = HeapHeader{};
    header_.dim = info_.vector_col >= 0 ? static_cast<uint32_t>(info_.columns[info_.vector_col].vector_dim) : 0;
    header_.next_page = 1;
    header_.computeCrc();
    writeHeaderToPage();
  }
}

void Table::writeHeaderToPage() {
  header_.computeCrc();
  PageGuard g(pool_, pool_->fetch(heap_fid_, 0));
  eng_->noteWrite(oid_, 0, g.data());
  std::memcpy(g.data(), &header_, sizeof(header_));
  pool_->markDirty(g.frame());
}

void Table::initialize(bool create) {
  openFiles(create);
}

void Table::prepareAfterRedo() {
  readHeader();
  if (info_.vector_col >= 0)
    header_.dim = static_cast<uint32_t>(info_.columns[info_.vector_col].vector_dim);
  scan_hint_ = header_.next_page > 1 ? header_.next_page - 1 : 1;
  vidx_.reset(static_cast<uint32_t>(header_.dim));
  if (cfg_->ivf_nlist > 0) vidx_.setNlist(cfg_->ivf_nlist);
  loadSidecars();
}

size_t Table::maxRecordSize() const {
  size_t n = kRecordPrefix + sizeof(uint32_t) * (info_.columns.size() + 1);
  for (const Column& c : info_.columns) {
    switch (c.type) {
      case DataType::Bigint: n += 8; break;
      case DataType::Varchar: n += static_cast<size_t>(c.varchar_len) * 4; break;
      case DataType::Vector: n += static_cast<size_t>(c.vector_dim) * 4; break;
    }
  }
  return n;
}

bool Table::allocSlot(const uint8_t* rec, uint16_t len, Rid& out) {
  const size_t ps = pool_->pageSize();
  const uint32_t from = std::min<uint32_t>(scan_hint_, header_.next_page ? header_.next_page - 1 : 0);
  // Reuse space freed by deletes before growing the file: look at a bounded
  // window of pages so an insert never degenerates into a full scan.
  for (uint32_t attempt = 0; attempt < 2; ++attempt) {
    uint32_t start = attempt == 0 ? from : 1;
    uint32_t stop = attempt == 0 ? header_.next_page : std::min<uint32_t>(header_.next_page, from + 64);
    for (uint32_t p = start; p < stop; ++p) {
      PageGuard g(pool_, pool_->fetch(heap_fid_, p));
      const PageHeader& ph = headerOf(g.data());
      if (ph.type != kPageHeap) continue;
      if (pageFreeSpace(g.data()) < len + sizeof(Slot)) continue;
      eng_->noteWrite(oid_, p, g.data());
      int idx = pageInsertRecord(g.data(), ps, rec, len);
      if (idx < 0) continue;
      pool_->markDirty(g.frame());
      scan_hint_ = p;
      out = Rid{p, static_cast<uint16_t>(idx)};
      return true;
    }
    if (from == 0) break;
  }

  // Grow the file by one page.
  uint32_t p = header_.next_page;
  if (p == 0) p = 1;
  PageGuard g(pool_, pool_->fetch(heap_fid_, p));
  eng_->noteWrite(oid_, p, g.data());
  initPage(g.data(), ps, kPageHeap);
  pool_->markDirty(g.frame());
  int idx = pageInsertRecord(g.data(), ps, rec, len);
  if (idx < 0) return false;  // row does not fit an empty page
  header_.next_page = p + 1;
  scan_hint_ = p;
  out = Rid{p, static_cast<uint16_t>(idx)};
  return true;
}

void Table::markSlotDead(Rid rid) {
  PageGuard g(pool_, pool_->fetch(heap_fid_, rid.page));
  const PageHeader& ph = headerOf(g.data());
  if (ph.type != kPageHeap || rid.slot >= ph.slot_count) return;
  eng_->noteWrite(oid_, rid.page, g.data());
  pageDeleteRecord(g.data(), pool_->pageSize(), rid.slot);
  pool_->markDirty(g.frame());
}

bool Table::readRecord(Rid rid, Row& out) const {
  if (rid.page == 0 || rid.page >= header_.next_page) return false;
  PageGuard g(pool_, pool_->fetch(heap_fid_, rid.page));
  const PageHeader& h = headerOf(g.data());
  if (h.type != kPageHeap || rid.slot >= h.slot_count) return false;
  const Slot& s = slotAt(g.data(), rid.slot);
  if (!(s.flags & kSlotLive)) return false;
  decodeRecord(g.data() + s.off, info_.columns, out);
  return true;
}

bool Table::readPkOf(Rid rid, int64_t& pk) const {
  if (rid.page == 0 || rid.page >= header_.next_page) return false;
  PageGuard g(pool_, pool_->fetch(heap_fid_, rid.page));
  const PageHeader& h = headerOf(g.data());
  if (h.type != kPageHeap || rid.slot >= h.slot_count) return false;
  const Slot& s = slotAt(g.data(), rid.slot);
  if (!(s.flags & kSlotLive)) return false;
  pk = recordBigint(g.data() + s.off, static_cast<size_t>(info_.pk_col));
  return true;
}

bool Table::findPk(int64_t pk, Rid& out, const Overlay* stmt, const Overlay* tx) const {
  if (stmt) {
    auto it = stmt->pk_set.find(pk);
    if (it != stmt->pk_set.end()) {
      out = it->second;
      return true;
    }
    if (stmt->pk_del.count(pk)) return false;
  }
  if (tx) {
    auto it = tx->pk_set.find(pk);
    if (it != tx->pk_set.end()) {
      out = it->second;
      return true;
    }
    if (tx->pk_del.count(pk)) return false;
  }
  auto it = pk_.find(pk);
  if (it == pk_.end()) return false;
  out = it->second;
  return true;
}

bool Table::insertRow(const Row& row, Overlay& ov, std::string& err) {
  const int64_t pk = row[static_cast<size_t>(info_.pk_col)].i;
  std::vector<uint8_t> buf;
  buf.resize(pool_->pageSize());
  size_t n = recordEncodedSize(row, info_.columns);
  if (n + sizeof(Slot) > pool_->pageSize() - sizeof(PageHeader)) {
    err = "row of " + std::to_string(n) + " bytes does not fit a page";
    return false;
  }
  buf.resize(n);
  encodeRecord(row, info_.columns, buf.data(), n);
  Rid rid;
  if (!allocSlot(buf.data(), static_cast<uint16_t>(n), rid)) {
    err = "no page space for a " + std::to_string(n) + "-byte row";
    return false;
  }
  header_.row_count += 1;
  writeHeaderToPage();
  ov.pk_set[pk] = rid;
  ov.pk_del.erase(pk);
  if (info_.vector_col >= 0) ov.vec_add[pk] = row[static_cast<size_t>(info_.vector_col)].v;
  return true;
}

bool Table::deleteRow(Rid rid, int64_t pk, Overlay& ov, std::string& err) {
  PageGuard g(pool_, pool_->fetch(heap_fid_, rid.page));
  const PageHeader& h = headerOf(g.data());
  if (h.type != kPageHeap || rid.slot >= h.slot_count || !(slotAt(g.data(), rid.slot).flags & kSlotLive)) {
    err = "record vanished under the delete";
    return false;
  }
  eng_->noteWrite(oid_, rid.page, g.data());
  pageDeleteRecord(g.data(), pool_->pageSize(), rid.slot);
  pool_->markDirty(g.frame());
  header_.row_count -= 1;
  writeHeaderToPage();
  ov.pk_del.insert(pk);
  ov.pk_set.erase(pk);
  ov.vec_del.insert(pk);
  ov.vec_add.erase(pk);
  return true;
}

bool Table::rewriteRow(Rid old_rid, const Row& newrow, Overlay& ov, std::string& err) {
  const int64_t pk = newrow[static_cast<size_t>(info_.pk_col)].i;
  size_t n = recordEncodedSize(newrow, info_.columns);
  std::vector<uint8_t> buf(n);
  encodeRecord(newrow, info_.columns, buf.data(), n);

  // In place when the image fits the existing slot, otherwise append and
  // reclaim. Either way the primary key keeps pointing at exactly one live
  // record, which is what the visible-set contract requires.
  bool inplace = false;
  {
    PageGuard g(pool_, pool_->fetch(heap_fid_, old_rid.page));
    const PageHeader& h = headerOf(g.data());
    if (h.type == kPageHeap && old_rid.slot < h.slot_count &&
        (slotAt(g.data(), old_rid.slot).flags & kSlotLive) &&
        slotAt(g.data(), old_rid.slot).len >= n) {
      eng_->noteWrite(oid_, old_rid.page, g.data());
      pageUpdateRecord(g.data(), pool_->pageSize(), old_rid.slot, buf.data(),
                       static_cast<uint16_t>(n));
      pool_->markDirty(g.frame());
      inplace = true;
    }
  }
  Rid rid;
  if (inplace) {
    rid = old_rid;
  } else {
    markSlotDead(old_rid);
    if (!allocSlot(buf.data(), static_cast<uint16_t>(n), rid)) {
      err = "no page space for the updated row";
      return false;
    }
  }
  ov.pk_set[pk] = rid;
  ov.pk_del.erase(pk);
  if (info_.vector_col >= 0) {
    ov.vec_del.insert(pk);
    ov.vec_add[pk] = newrow[static_cast<size_t>(info_.vector_col)].v;
  }
  return true;
}

void Table::applyOverlay(const Overlay& ov) {
  Overlay copy = ov;
  for (int64_t k : copy.pk_del) {
    copy.pk_set.erase(k);
    copy.vec_add.erase(k);
  }
  for (int64_t k : copy.pk_del) {
    pk_.erase(k);
    vidx_.remove(k);
  }
  for (int64_t k : copy.vec_del) {
    if (copy.pk_del.count(k)) continue;
    if (copy.vec_add.count(k)) continue;  // replaced below
    vidx_.remove(k);
  }
  for (const auto& kv : copy.vec_add) {
    vidx_.remove(kv.first);
    vidx_.insert(kv.first, kv.second.data());
  }
  for (const auto& kv : copy.pk_set) pk_[kv.first] = kv.second;
}

void Table::topKAnn(const float* query, size_t dim, size_t k, const Overlay* stmt,
                    const Overlay* tx, std::vector<std::pair<Rid, double>>& out) const {
  out.clear();
  if (k == 0) return;
  if (info_.vector_col < 0)
    throw SqlError(ErrKind::TypeMismatch, "table has no vector column");
  const size_t vcol = static_cast<size_t>(info_.vector_col);
  const size_t pkcol = static_cast<size_t>(info_.pk_col);

  if (!indexBuilt() || !vidx_.built() || vidx_.dim() != dim) {
    // No usable index: fall back to an exhaustive scan, but keep the bounded
    // heap. Materialising and sorting the whole table here was slower than
    // the exact executor, which made "ann without an index" a penalty rather
    // than a neutral choice.
    std::vector<TopKCand> best;
    best.reserve(k);
    scan([&](const uint8_t* rec, Rid rid) {
      TopKCand c;
      c.rid = rid;
      c.pk = recordBigint(rec, pkcol);
      c.dist = l2Squared(query, recordVector(rec, vcol), dim);
      if (best.size() < k) {
        best.push_back(c);
        if (best.size() == k) std::make_heap(best.begin(), best.end(), candBetter);
      } else if (candBetter(c, best.front())) {
        std::pop_heap(best.begin(), best.end(), candBetter);
        best.back() = c;
        std::push_heap(best.begin(), best.end(), candBetter);
      }
      return true;
    });
    sortCands(best);
    out.reserve(best.size());
    for (const auto& c : best) out.emplace_back(c.rid, c.dist);
    return;
  }

  std::vector<VecHit> hits;
  vidx_.search(query, k, hits, cfg_->ivf_nprobe);

  auto overridden = [&](int64_t id) {
    if (tx && (tx->vec_del.count(id) || tx->vec_add.count(id))) return true;
    if (stmt && (stmt->vec_del.count(id) || stmt->vec_add.count(id))) return true;
    return false;
  };

  std::vector<TopKCand> best;
  best.reserve(2 * k);
  for (const auto& h : hits) {
    if (overridden(h.id)) continue;
    Rid r;
    if (!findPk(h.id, r, stmt, tx)) continue;
    best.push_back(TopKCand{r, h.id, static_cast<double>(h.dist)});
  }
  auto addPending = [&](const Overlay& ov) {
    for (const auto& kv : ov.vec_add) {
      if (ov.vec_del.count(kv.first)) {
        // Deleted again after the pending write; only a later re-insert keeps
        // the key alive, which findPk resolves.
      }
      Rid r;
      if (!findPk(kv.first, r, stmt, tx)) continue;
      if (kv.second.size() != dim) continue;
      TopKCand c;
      c.rid = r;
      c.pk = kv.first;
      c.dist = l2Squared(query, kv.second.data(), dim);
      bool dup = false;
      for (const auto& e : best)
        if (e.rid == r) dup = true;
      if (dup) continue;
      best.push_back(c);
    }
  };
  if (tx) addPending(*tx);
  if (stmt) addPending(*stmt);

  sortCands(best);
  if (best.size() > k) best.resize(k);
  out.reserve(best.size());
  for (const auto& c : best) out.emplace_back(c.rid, c.dist);
}

// ---------------------------------------------------------------------------
// Sidecars: persisted primary-key and vector indexes
// ---------------------------------------------------------------------------

static const uint32_t kPkiMagic = 0x4D565031u;  // "MVP1"
static const uint32_t kVidxMagic = 0x4D564931u;

void Table::saveSidecars() {
  if (header_.row_count == 0) {
    ::unlink(sidecarPath(cfg_, oid_, ".pki").c_str());
    ::unlink(sidecarPath(cfg_, oid_, ".vidx").c_str());
    return;
  }
  {
    std::vector<uint8_t> buf;
    auto put = [&](const void* p, size_t n) {
      const uint8_t* b = static_cast<const uint8_t*>(p);
      buf.insert(buf.end(), b, b + n);
    };
    uint32_t m = kPkiMagic, v = 1;
    put(&m, 4);
    put(&v, 4);
    uint64_t rc = header_.row_count, ep = header_.epoch, cnt = pk_.size();
    put(&rc, 8);
    put(&ep, 8);
    put(&cnt, 8);
    for (const auto& kv : pk_) {
      int64_t pk = kv.first;
      uint32_t page = kv.second.page;
      uint16_t slot = kv.second.slot;
      put(&pk, 8);
      put(&page, 4);
      put(&slot, 2);
    }
    writeFileAll(sidecarPath(cfg_, oid_, ".pki"), buf.data(), buf.size());
  }
  if (indexBuilt() && vidx_.built()) {
    std::vector<uint8_t> body = vidx_.serialize();
    std::vector<uint8_t> buf;
    uint32_t m = kVidxMagic;
    uint64_t rc = header_.row_count, ep = header_.epoch;
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&m), reinterpret_cast<uint8_t*>(&m) + 4);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rc), reinterpret_cast<uint8_t*>(&rc) + 8);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 8);
    buf.insert(buf.end(), body.begin(), body.end());
    writeFileAll(sidecarPath(cfg_, oid_, ".vidx"), buf.data(), buf.size());
  } else {
    ::unlink(sidecarPath(cfg_, oid_, ".vidx").c_str());
  }
}

void Table::loadSidecars() {
  bool ok = false;
  {
    std::vector<uint8_t> buf;
    if (readFileAll(sidecarPath(cfg_, oid_, ".pki"), buf) && buf.size() >= 32) {
      size_t off = 0;
      auto take = [&](void* dst, size_t n) {
        if (off + n > buf.size()) return false;
        std::memcpy(dst, buf.data() + off, n);
        off += n;
        return true;
      };
      uint32_t m = 0, v = 0;
      uint64_t rc = 0, ep = 0, cnt = 0;
      if (take(&m, 4) && m == kPkiMagic && take(&v, 4) && v == 1 && take(&rc, 8) && take(&ep, 8) &&
          take(&cnt, 8) && rc == header_.row_count && ep == header_.epoch) {
        pk_.clear();
        bool full = true;
        for (uint64_t i = 0; i < cnt; ++i) {
          int64_t pk;
          uint32_t page;
          uint16_t slot;
          if (!take(&pk, 8) || !take(&page, 4) || !take(&slot, 2)) {
            full = false;
            break;
          }
          pk_[pk] = Rid{page, slot};
        }
        if (full && pk_.size() == cnt && header_.row_count == cnt) ok = true;
      }
    }
  }
  if (ok && indexBuilt()) {
    std::vector<uint8_t> buf;
    bool v_ok = false;
    if (readFileAll(sidecarPath(cfg_, oid_, ".vidx"), buf) && buf.size() >= 20) {
      size_t off = 0;
      uint32_t m = 0;
      uint64_t rc = 0, ep = 0;
      std::memcpy(&m, buf.data(), 4);
      std::memcpy(&rc, buf.data() + 4, 8);
      std::memcpy(&ep, buf.data() + 12, 8);
      off = 20;
      if (m == kVidxMagic && rc == header_.row_count && ep == header_.epoch)
        v_ok = vidx_.deserialize(buf.data() + off, buf.size() - off,
                                 static_cast<uint32_t>(header_.dim));
    }
    if (!v_ok) ok = false;
  } else if (ok && !indexBuilt()) {
    std::vector<uint8_t> buf;
    if (readFileAll(sidecarPath(cfg_, oid_, ".vidx"), buf)) ok = false;
  }

  if (!ok) rebuildIndexes();
}

void Table::rebuildIndexes() {
  pk_.clear();
  vidx_.reset(static_cast<uint32_t>(header_.dim));
  std::vector<int64_t> ids;
  std::vector<float> vecs;
  if (indexBuilt() && info_.vector_col >= 0) {
    ids.reserve(static_cast<size_t>(header_.row_count));
    vecs.reserve(static_cast<size_t>(header_.row_count) * header_.dim);
  }
  scan([&](const uint8_t* rec, Rid rid) {
    int64_t pk = recordBigint(rec, static_cast<size_t>(info_.pk_col));
    pk_[pk] = rid;
    if (indexBuilt() && info_.vector_col >= 0) {
      ids.push_back(pk);
      const float* v = recordVector(rec, static_cast<size_t>(info_.vector_col));
      vecs.insert(vecs.end(), v, v + header_.dim);
    }
    return true;
  });
  if (indexBuilt() && info_.vector_col >= 0) vidx_.build(ids, vecs);
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine(Config cfg) : cfg_(std::move(cfg)) {
  pool_ = std::make_unique<BufferPool>(cfg_.page_size, cfg_.pool_pages);
  catalog_path_ = joinPath(cfg_.data_dir, "catalog.txt");
  // The CLI switch and SET share one variable; without this the ann path is
  // unreachable from the command line and the approximate index never runs.
  ann_mode_ = cfg_.vector_mode == Config::VectorMode::Ann;
}

Engine::~Engine() = default;

Table* Engine::table(const std::string& name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second.get();
}

Table* Engine::tableByOid(uint32_t oid) {
  auto it = by_oid_.find(oid);
  return it == by_oid_.end() ? nullptr : it->second;
}

Table* Engine::requireTable(const std::string& name) {
  Table* t = table(name);
  if (!t) throw SqlError(ErrKind::NoSuchTable, "table '" + name + "' does not exist");
  return t;
}

int Engine::columnIndex(const TableInfo& ti, const std::string& name) const {
  for (size_t i = 0; i < ti.columns.size(); ++i)
    if (ti.columns[i].name == name) return static_cast<int>(i);
  return -1;
}

void Engine::startup() {
  double t0 = nowMs();
  mkdirp(cfg_.data_dir);
  loadCatalog();

  // Order matters. Opening the files must not read or write a page, because
  // the redo pass below pwrites straight into them; if a table touched its
  // header first, that page would land in the buffer pool dirty and a later
  // flush would overwrite the recovered copy with a fresh empty one.
  for (auto& kv : tables_) kv.second->initialize(true);

  double r0 = nowMs();
  std::string wal_path = joinPath(cfg_.data_dir, "mvd.wal");
  auto sr = Wal::scan(wal_path, cfg_.page_size);
  if (!sr.committed.empty()) {
    Wal tmp;
    tmp.open(wal_path);
    std::unordered_map<uint32_t, uint32_t> oid_to_fid;
    for (auto& kv : tables_) oid_to_fid[kv.second->oid()] = kv.second->heapFileId();
    stats_.recovered_pages = tmp.applyRedo(
        wal_path, cfg_.page_size, sr.committed, sr.base_lsn,
        [&](uint32_t oid, int& fd) {
          auto it = oid_to_fid.find(oid);
          if (it == oid_to_fid.end()) return false;
          fd = pool_->fileFd(it->second);
          return fd >= 0;
        });
    for (auto& kv : tables_) ::fsync(pool_->fileFd(kv.second->heapFileId()));
    tmp.close();
  }
  stats_.recovered_txns = sr.committed.size();
  stats_.next_lsn_after_recovery = sr.next_lsn;

  wal_.open(wal_path);
  // Every committed image is now in the data files and every uncommitted
  // record is by construction ignorable, so the log can always start fresh.
  // Doing this unconditionally also keeps next_lsn_ from restarting at 1
  // while stale records still sit in the file.
  wal_.resetTo(0, sr.next_lsn);

  for (auto& kv : tables_) kv.second->prepareAfterRedo();
  stats_.recovery_ms = nowMs() - r0;
  stats_.startup_ms = nowMs() - t0;
}

void Engine::shutdown() {
  try {
    if (in_txn_) rollbackTxn();
    pool_->flushAll();
    for (auto& kv : tables_) kv.second->fsyncData();
    for (auto& kv : tables_) kv.second->saveSidecars();
    writeCatalog();
    if (wal_.open_()) {
      wal_.appendCheckpoint(wal_.nextLsn(), wal_.nextLsn());
      wal_.sync();
      wal_.resetTo(0, wal_.nextLsn());
      wal_.close();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "shutdown error: %s\n", e.what());
  }
}

void Engine::noteWrite(uint32_t oid, uint32_t page, const uint8_t* before) {
  PageRef r{tableByOid(oid) ? tableByOid(oid)->heapFileId() : 0, page};
  if (stmt_undo_.find(r) == stmt_undo_.end())
    stmt_undo_.emplace(r, std::vector<uint8_t>(before, before + cfg_.page_size));
  if (tx_undo_.find(r) == tx_undo_.end())
    tx_undo_.emplace(r, std::vector<uint8_t>(before, before + cfg_.page_size));
  if (txn_dirty_set_.insert(r).second) txn_dirty_.push_back(r);
}

void Engine::restoreUndo(std::unordered_map<PageRef, std::vector<uint8_t>, PageRefHash>& map) {
  for (auto& kv : map) {
    Frame* f = pool_->fetch(kv.first.file, kv.first.page);
    std::memcpy(f->data, kv.second.data(), cfg_.page_size);
    pool_->markDirty(f);
    pool_->unpin(f);
  }
  std::unordered_set<uint32_t> oids;
  for (const auto& kv : map)
    for (auto& t : tables_)
      if (t.second->heapFileId() == kv.first.file) oids.insert(t.second->oid());
  map.clear();
  // Page 0 carries the row counter and the page high-water mark, so restoring
  // it must also refresh the in-memory copy.
  for (uint32_t oid : oids) {
    Table* t = tableByOid(oid);
    if (t) t->readHeader();
  }
}

void Engine::beginStatement() {
  statement_active_ = true;
  if (!in_txn_) {
    xid_ = next_xid_++;
    pool_->setActiveXid(xid_);
  }
}

void Engine::finishStatement() {
  for (auto& kv : stmt_overlay_) tx_overlay_[kv.first].mergeFrom(kv.second);
  stmt_overlay_.clear();
  stmt_undo_.clear();
  if (!in_txn_) commitNow();
  statement_active_ = false;
}

void Engine::abortStatement() {
  restoreUndo(stmt_undo_);
  stmt_undo_.clear();
  stmt_overlay_.clear();
  if (!in_txn_) {
    // The whole implicit transaction is this one statement.
    restoreUndo(tx_undo_);
    tx_undo_.clear();
    tx_overlay_.clear();
    txn_dirty_.clear();
    txn_dirty_set_.clear();
    xid_ = 0;
    pool_->setActiveXid(0);
  }
  statement_active_ = false;
}

void Engine::commitNow() {
  if (txn_dirty_.empty() && tx_overlay_.empty()) {
    // Nothing to persist; still close the transaction slot.
    xid_ = 0;
    pool_->setActiveXid(0);
    tx_undo_.clear();
    tx_overlay_.clear();
    txn_dirty_.clear();
    txn_dirty_set_.clear();
    return;
  }
  for (const PageRef& r : txn_dirty_) {
    Frame* f = pool_->fetch(r.file, r.page);
    uint32_t oid = 0;
    for (auto& kv : tables_)
      if (kv.second->heapFileId() == r.file) oid = kv.second->oid();
    wal_.appendRedo(oid, r.page, f->data, cfg_.page_size);
    stats_.redo_pages += 1;
    pool_->unpin(f);
  }
  wal_.appendCommit(xid_);
  wal_.sync();

  for (auto& kv : tx_overlay_) {
    Table* t = tableByOid(kv.first);
    if (t) t->applyOverlay(kv.second);
  }
  for (auto& kv : tx_overlay_) {
    Table* t = tableByOid(kv.first);
    if (t && !kv.second.empty()) {
      t->header().epoch += 1;
      t->writeHeaderToPage();
    }
  }
  tx_overlay_.clear();
  tx_undo_.clear();
  txn_dirty_.clear();
  txn_dirty_set_.clear();
  xid_ = 0;
  pool_->setActiveXid(0);
  stats_.commits += 1;
  checkpoint(false);
}

void Engine::beginTxn() {
  if (in_txn_) throw SqlError(ErrKind::TxnState, "nested transactions are not supported");
  in_txn_ = true;
  xid_ = next_xid_++;
  pool_->setActiveXid(xid_);
}

void Engine::commitTxn() {
  if (!in_txn_) return;  // autocommit: nothing open
  in_txn_ = false;
  commitNow();
}

void Engine::rollbackTxn() {
  if (!in_txn_) return;
  in_txn_ = false;
  restoreUndo(tx_undo_);
  tx_undo_.clear();
  stmt_undo_.clear();
  tx_overlay_.clear();
  stmt_overlay_.clear();
  txn_dirty_.clear();
  txn_dirty_set_.clear();
  xid_ = 0;
  pool_->setActiveXid(0);
}

void Engine::checkpoint(bool force) {
  size_t wsz = wal_.sizeBytes();
  if (!force && wsz < cfg_.checkpoint_bytes) return;
  pool_->flushAll();
  for (auto& kv : tables_) kv.second->fsyncData();
  wal_.appendCheckpoint(wal_.nextLsn(), wal_.nextLsn());
  wal_.sync();
  wal_.resetTo(0, wal_.nextLsn());
  for (auto& kv : tables_) kv.second->saveSidecars();
  stats_.checkpoints += 1;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

void Engine::writeCatalog() {
  std::ostringstream os;
  os << "# MiniVectorDB catalog v1\n";
  os << "page_size " << cfg_.page_size << "\n";
  os << "next_oid " << next_oid_ << "\n";
  for (const auto& kv : tables_) {
    const TableInfo& ti = kv.second->info();
    os << "table " << kv.second->oid() << " " << ti.name << "\n";
    for (size_t i = 0; i < ti.columns.size(); ++i) {
      const Column& c = ti.columns[i];
      const char* tn = c.type == DataType::Bigint    ? "BIGINT"
                       : c.type == DataType::Varchar ? "VARCHAR"
                                                     : "VECTOR";
      os << "col " << c.name << " " << tn << " " << c.varchar_len << " " << c.vector_dim << " "
         << (static_cast<int>(i) == ti.pk_col ? "PK" : "-") << "\n";
    }
    os << "index " << (kv.second->indexBuilt() ? 1 : 0) << "\n";
  }
  std::string text = os.str();
  std::string tmp = catalog_path_ + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw std::runtime_error("catalog open failed");
  size_t off = 0;
  while (off < text.size()) {
    ssize_t w = ::write(fd, text.data() + off, text.size() - off);
    if (w <= 0) {
      ::close(fd);
      throw std::runtime_error("catalog write failed");
    }
    off += static_cast<size_t>(w);
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), catalog_path_.c_str()) != 0)
    throw std::runtime_error("catalog rename failed");
  // Renaming into place only survives a crash if the directory entry is
  // synced as well.
  int dfd = ::open(cfg_.data_dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  catalog_dirty_ = false;
}

void Engine::loadCatalog() {
  std::ifstream in(catalog_path_);
  if (!in) return;
  std::string line;
  std::unique_ptr<Table> pending;

  auto commitPending = [&]() {
    if (!pending) return;
    pending->setEngine(this);
    Table* raw = pending.get();
    tables_[raw->info().name] = std::move(pending);
    by_oid_[raw->oid()] = raw;
    pending.reset();
  };

  while (std::getline(in, line)) {
    std::vector<std::string> tk;
    {
      std::istringstream is(line);
      std::string w;
      while (is >> w) tk.push_back(w);
    }
    if (tk.empty() || tk[0][0] == '#') continue;
    if (tk[0] == "page_size") {
      continue;  // fixed by the running configuration
    }
    if (tk[0] == "next_oid" && tk.size() > 1) {
      next_oid_ = static_cast<uint32_t>(std::stoul(tk[1]));
      continue;
    }
    if (tk[0] == "table" && tk.size() >= 3) {
      commitPending();
      TableInfo ti;
      ti.oid = static_cast<uint32_t>(std::stoul(tk[1]));
      ti.name = tk[2];
      pending = std::make_unique<Table>(std::move(ti), pool_.get(), &cfg_);
      pending->setOid(ti.oid);
      continue;
    }
    if (tk[0] == "col" && pending && tk.size() >= 6) {
      Column c;
      c.name = tk[1];
      c.type = tk[2] == "BIGINT" ? DataType::Bigint
               : tk[2] == "VARCHAR" ? DataType::Varchar
                                    : DataType::Vector;
      c.varchar_len = std::stoll(tk[3]);
      c.vector_dim = std::stoll(tk[4]);
      TableInfo& ti = pending->infoMut();
      ti.columns.push_back(std::move(c));
      if (tk[5] == "PK") ti.pk_col = static_cast<int>(ti.columns.size() - 1);
      if (ti.columns.back().type == DataType::Vector)
        ti.vector_col = static_cast<int>(ti.columns.size() - 1);
      continue;
    }
    if (tk[0] == "index" && pending && tk.size() >= 2) {
      pending->markIndexBuilt(tk[1] == "1");
      continue;
    }
  }
  commitPending();
}

}  // namespace mvd
