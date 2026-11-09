#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"
#include "index/vecindex.h"
#include "sql/ast.h"
#include "storage/buffer.h"
#include "storage/page.h"
#include "storage/wal.h"

namespace mvd {

struct Cell {
  enum Kind : uint8_t { Int = 0, Real = 1, Str = 2, Vec = 3 } kind = Int;
  int64_t i = 0;
  double r = 0;
  std::string s;
  std::vector<float> v;

  static Cell ofInt(int64_t x) { Cell c; c.kind = Int; c.i = x; return c; }
  static Cell ofReal(double x) { Cell c; c.kind = Real; c.r = x; return c; }
  static Cell ofStr(std::string x) { Cell c; c.kind = Str; c.s = std::move(x); return c; }
  static Cell ofVec(std::vector<float> x) { Cell c; c.kind = Vec; c.v = std::move(x); return c; }
  static Cell of(const Value& v) {
    switch (v.type) {
      case DataType::Bigint: return ofInt(v.i);
      case DataType::Varchar: return ofStr(v.s);
      case DataType::Vector: return ofVec(v.v);
      case DataType::Real: return ofReal(v.r);
    }
    return Cell{};
  }
};

struct QueryResult {
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
  int64_t affected = -1;  // -1 = nothing to report
};

// Page 0 of every heap file.
//
// Every byte of this struct is a named field: the CRC covers the whole object
// representation, so an unnamed tail padding byte would make the check fail
// at random and a valid header would be discarded as corrupt.
struct HeapHeader {
  uint32_t magic = 0x4D564831u;  // "MVH1"
  uint32_t version = 1;
  uint64_t row_count = 0;
  uint32_t next_page = 1;  // page 0 is the header
  uint32_t dim = 0;
  uint64_t epoch = 0;      // bumped by every commit that changed row data
  uint32_t index_built = 0;
  uint32_t reserved = 0;
  uint32_t crc = 0;
  uint32_t tail = 0;       // explicit padding: keeps sizeof() fully defined

  void computeCrc();
  bool checkCrc() const;
};

static_assert(sizeof(HeapHeader) % 8 == 0, "HeapHeader must have no unnamed padding");

// Primary-key and vector-index mutations that are not visible to the base
// structures until the enclosing statement, and then the enclosing
// transaction, succeed.
struct Overlay {
  std::unordered_map<int64_t, Rid> pk_set;
  std::unordered_set<int64_t> pk_del;
  // Last write per key wins, so an update inside a transaction does not leave
  // the superseded vector behind.
  std::unordered_map<int64_t, std::vector<float>> vec_add;
  std::unordered_set<int64_t> vec_del;

  void clear() {
    pk_set.clear();
    pk_del.clear();
    vec_add.clear();
    vec_del.clear();
  }
  bool empty() const { return pk_set.empty() && pk_del.empty() && vec_add.empty() && vec_del.empty(); }
  void mergeFrom(const Overlay& o) {
    for (const auto& kv : o.pk_set) pk_set[kv.first] = kv.second;
    for (int64_t k : o.pk_del) pk_del.insert(k);
    for (const auto& kv : o.vec_add) vec_add[kv.first] = kv.second;
    for (int64_t k : o.vec_del) vec_del.insert(k);
    // A key deleted by a later statement must not survive as a pending add.
    for (int64_t k : o.pk_del) {
      pk_set.erase(k);
      vec_add.erase(k);
    }
  }
};

class Table {
 public:
  Table(TableInfo info, BufferPool* pool, const Config* cfg)
      : info_(std::move(info)), pool_(pool), cfg_(cfg) {}

  const TableInfo& info() const { return info_; }
  TableInfo& infoMut() { return info_; }
  void setInfo(TableInfo i) { info_ = std::move(i); }

  void openFiles(bool create);
  void closeFiles();
  void dropFiles();

  uint32_t oid() const { return oid_; }
  void setOid(uint32_t o) { oid_ = o; }
  class Engine* engine() const { return eng_; }
  void setEngine(class Engine* e) { eng_ = e; }
  uint32_t heapFileId() const { return heap_fid_; }
  uint32_t dataPages() const { return header_.next_page; }
  uint64_t rowCount() const { return header_.row_count; }
  uint64_t epoch() const { return header_.epoch; }
  bool indexBuilt() const { return header_.index_built != 0; }
  void markIndexBuilt(bool b) { header_.index_built = b ? 1 : 0; }
  HeapHeader& header() { return header_; }
  const HeapHeader& header() const { return header_; }

  void readHeader();
  void writeHeaderToPage();  // stages the header in the buffer pool
  void initialize(bool create);
  // Reads the header and loads or rebuilds the indexes. Must run only after
  // the WAL redo pass, otherwise the recovered pages get clobbered.
  void prepareAfterRedo();

  // Visits every live record image. `visit` returns false to stop the scan.
  template <typename F>
  bool scan(F&& visit) const {
    const uint32_t npages = header_.next_page;
    for (uint32_t p = 1; p < npages; ++p) {
      PageGuard g(pool_, pool_->fetch(heap_fid_, p));
      const uint8_t* page = g.data();
      const PageHeader& h = headerOf(page);
      if (h.type != kPageHeap) continue;
      for (uint16_t s = 0; s < h.slot_count; ++s) {
        const Slot& sl = slotAt(page, s);
        if (!(sl.flags & kSlotLive)) continue;
        if (!visit(page + sl.off, Rid{p, s})) return false;
      }
    }
    return true;
  }

  bool readRecord(Rid rid, Row& out) const;
  bool readPkOf(Rid rid, int64_t& pk) const;

  // Resolves a primary key through the statement overlay, then the
  // transaction overlay, then the base index.
  bool findPk(int64_t pk, Rid& out, const Overlay* stmt, const Overlay* tx) const;

  bool insertRow(const Row& row, Overlay& ov, std::string& err);
  bool deleteRow(Rid rid, int64_t pk, Overlay& ov, std::string& err);
  bool rewriteRow(Rid old_rid, const Row& newrow, Overlay& ov, std::string& err);

  void applyOverlay(const Overlay& ov);

  const VectorIndex& vecIndex() const { return vidx_; }
  VectorIndex& vecIndex() { return vidx_; }
  const std::unordered_map<int64_t, Rid>& pkBase() const { return pk_; }

  // TopK over the currently visible set.
  //
  // Exact: one heap pass, predicate evaluated before the distance, ties broken
  // by ascending primary key. This is also what a filtered approximate query
  // runs, because evaluating a scalar predicate already costs a visit to every
  // record, so the index could not skip the work that dominates the query.
  template <typename M>
  void topKExact(const float* query, size_t dim, size_t k, const M& matches,
                 std::vector<std::pair<Rid, double>>& out) const;

  // Approximate: IVF probe plus whatever the transaction added or removed on
  // top of the base index. Only valid without a scalar filter.
  void topKAnn(const float* query, size_t dim, size_t k, const Overlay* stmt, const Overlay* tx,
               std::vector<std::pair<Rid, double>>& out) const;

  void loadSidecars();
  void saveSidecars();
  void rebuildIndexes();
  void fsyncData();
  std::string heapPath() const;

  size_t maxRecordSize() const;
  int pkColumn() const { return info_.pk_col; }
  int vectorColumn() const { return info_.vector_col; }

 private:
  bool allocSlot(const uint8_t* rec, uint16_t len, Rid& out);
  void markSlotDead(Rid rid);
  const float* vectorOf(Rid rid) const;

  TableInfo info_;
  BufferPool* pool_;
  const Config* cfg_;
  class Engine* eng_ = nullptr;
  uint32_t oid_ = 0;
  uint32_t heap_fid_ = 0;
  uint32_t scan_hint_ = 1;
  uint32_t vidx_fid_ = 0;
  uint32_t pki_fid_ = 0;
  HeapHeader header_{};
  std::unordered_map<int64_t, Rid> pk_;
  VectorIndex vidx_;
};

// ---------------------------------------------------------------------------
// TopK
// ---------------------------------------------------------------------------

struct TopKCand {
  Rid rid;
  int64_t pk = 0;
  double dist = 0;
};

// Ordering the task fixes for L2 results: distance ascending, ties by
// ascending primary key.
inline bool candBetter(const TopKCand& a, const TopKCand& b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}

inline void sortCands(std::vector<TopKCand>& c) {
  std::sort(c.begin(), c.end(), candBetter);
}

template <typename M>
void Table::topKExact(const float* query, size_t dim, size_t k, const M& matches,
                      std::vector<std::pair<Rid, double>>& out) const {
  out.clear();
  if (k == 0) return;
  const size_t vcol = static_cast<size_t>(info_.vector_col);
  const size_t pkcol = static_cast<size_t>(info_.pk_col);
  std::vector<TopKCand> best;
  best.reserve(k);
  scan([&](const uint8_t* rec, Rid rid) {
    if (!matches(rec)) return true;
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
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

class Engine {
 public:
  explicit Engine(Config cfg);
  ~Engine();

  void startup();
  void shutdown();

  const Config& config() const { return cfg_; }
  BufferPool& pool() { return *pool_; }

  Table* table(const std::string& name);
  Table* tableByOid(uint32_t oid);
  const std::map<std::string, std::unique_ptr<Table>>& tables() const { return tables_; }

  void execute(const Statement&, QueryResult&);

  void beginTxn();
  void commitTxn();
  void rollbackTxn();
  bool inTxn() const { return in_txn_; }

  void setVectorMode(bool ann) { ann_mode_ = ann; }
  bool vectorMode() const { return ann_mode_; }

  // Called by Table immediately before a page image is modified: snapshots the
  // old bytes for statement/transaction undo and queues the page for the redo
  // log that commit will write.
  void noteWrite(uint32_t oid, uint32_t page, const uint8_t* before);

  struct Stats {
    uint64_t commits = 0;
    uint64_t checkpoints = 0;
    uint64_t redo_pages = 0;
    uint64_t recovered_pages = 0;
    uint64_t recovered_txns = 0;
    uint64_t next_lsn_after_recovery = 0;
    double startup_ms = 0;
    double recovery_ms = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void doCreateTable(const CreateTable&);
  void doDropTable(const DropTable&);
  void doInsert(const Insert&, QueryResult&);
  void doSelect(const Select&, QueryResult&);
  void doUpdate(const Update&, QueryResult&);
  void doDelete(const Delete&, QueryResult&);
  void doCreateIndex(const CreateVectorIndex&);
  void doSet(const SetStmt&);

  void beginStatement();
  void abortStatement();
  void finishStatement();
  void captureUndo(uint32_t oid, uint32_t page, const uint8_t* image);
  void restoreUndo(std::unordered_map<PageRef, std::vector<uint8_t>, PageRefHash>& map);
  void logDirty(PageRef r);
  void commitNow();
  void checkpoint(bool force);

  void writeCatalog();
  void loadCatalog();

  Table* requireTable(const std::string& name);
  int columnIndex(const TableInfo&, const std::string& name) const;
  Value coerce(const Expr&, const Column&, const TableInfo&) const;

  Config cfg_;
  std::unique_ptr<BufferPool> pool_;
  Wal wal_;
  std::map<std::string, std::unique_ptr<Table>> tables_;
  std::unordered_map<uint32_t, Table*> by_oid_;
  uint32_t next_oid_ = 1;

  uint32_t xid_ = 0;
  uint32_t next_xid_ = 1;
  bool in_txn_ = false;
  bool ann_mode_ = false;

  // Undo images, two levels: a statement that fails restores its own
  // first-touch copies; a ROLLBACK restores the copies taken at transaction
  // start. Both are captured before the buffer pool is modified, so a page
  // never needs in-place repair afterwards.
  std::unordered_map<PageRef, std::vector<uint8_t>, PageRefHash> stmt_undo_;
  std::unordered_map<PageRef, std::vector<uint8_t>, PageRefHash> tx_undo_;
  bool statement_active_ = false;

  // Index mutations staged per statement, then per transaction.
  std::unordered_map<uint32_t, Overlay> stmt_overlay_;
  std::unordered_map<uint32_t, Overlay> tx_overlay_;
  std::vector<PageRef> txn_dirty_;
  std::unordered_set<PageRef, PageRefHash> txn_dirty_set_;

  Overlay* overlayFor(uint32_t oid) { return &stmt_overlay_[oid]; }
  const Overlay* stmtOverlay(uint32_t oid) const {
    auto it = stmt_overlay_.find(oid);
    return it == stmt_overlay_.end() ? nullptr : &it->second;
  }
  const Overlay* txOverlay(uint32_t oid) const {
    auto it = tx_overlay_.find(oid);
    return it == tx_overlay_.end() ? nullptr : &it->second;
  }

  std::string catalog_path_;
  bool catalog_dirty_ = false;
  Stats stats_;
};

}  // namespace mvd
