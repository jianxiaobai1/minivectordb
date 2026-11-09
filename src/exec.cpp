#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <unordered_set>

#include "common/errors.h"
#include "common/fs.h"
#include "engine.h"
#include "sql/lexer.h"

namespace mvd {

namespace {

// Parses the textual vector form '[1,2,3]', which is how a vector arrives when
// the driver writes it as a quoted string.
std::vector<float> parseVectorText(const std::string& text) {
  std::vector<float> out;
  size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '[')) ++i;
  while (i < text.size()) {
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    std::string tok = text.substr(start, i - start);
    size_t lead = tok.find_first_not_of(" \t\n\r");
    size_t trail = tok.find_last_not_of(" \t\n\r");
    if (lead != std::string::npos) {
      tok = tok.substr(lead, trail - lead + 1);
      char* end = nullptr;
      double d = std::strtod(tok.c_str(), &end);
      if (end != tok.c_str() + tok.size())
        throw SqlError(ErrKind::InvalidVector, "vector component '" + tok + "' is not a number");
      if (d != d) throw SqlError(ErrKind::InvalidVector, "vector contains NaN");
      if (std::isinf(d) || d > std::numeric_limits<float>::max() ||
          d < -std::numeric_limits<float>::max())
        throw SqlError(ErrKind::InvalidVector, "vector component overflows float32");
      float f = static_cast<float>(d);
      if (std::isinf(f)) throw SqlError(ErrKind::InvalidVector, "vector contains Infinity");
      out.push_back(f);
    }
    if (i < text.size() && text[i] == ',') { ++i; continue; }
    break;
  }
  if (out.empty()) throw SqlError(ErrKind::VectorDimMismatch, "empty vector");
  return out;
}

bool truthy(const Value& v) {
  if (v.type == DataType::Bigint) return v.i != 0;
  if (v.type == DataType::Real) return v.r != 0.0;
  throw SqlError(ErrKind::TypeMismatch, "expression is not boolean");
}

bool numericValue(const Value& v) {
  return v.type == DataType::Bigint || v.type == DataType::Real;
}

double toReal(const Value& v) { return v.type == DataType::Real ? v.r : static_cast<double>(v.i); }

int cmpValues(const Value& a, const Value& b) {
  if (a.type == DataType::Varchar && b.type == DataType::Varchar) return utf8Compare(a.s, b.s);
  if (numericValue(a) && numericValue(b)) {
    // BIGINT and REAL compare numerically rather than by tag, so a floating
    // point threshold against an integer column or a distance works.
    double x = toReal(a), y = toReal(b);
    if (x != x || y != y) throw SqlError(ErrKind::TypeMismatch, "NaN cannot be compared");
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  throw SqlError(ErrKind::TypeMismatch, "these values cannot be compared");
}

struct EvalResult {
  bool is_real = false;
  Value val;
  double real = 0;
};

EvalResult evalGeneral(const Expr& e, const TableInfo& ti, const Row& row);

double evalReal(const Expr& e, const TableInfo& ti, const Row& row) {
  auto r = evalGeneral(e, ti, row);
  if (r.is_real) return r.real;
  if (r.val.type == DataType::Bigint) return static_cast<double>(r.val.i);
  if (r.val.type == DataType::Real) return r.val.r;
  throw SqlError(ErrKind::TypeMismatch, "expression is not numeric");
}

// Extracts the vector operand of L2_DISTANCE from an expression.
const Expr* vectorArgOf(const Expr& call, const Expr** col_out) {
  const Expr* a = call.args[0].get();
  const Expr* b = call.args.size() > 1 ? call.args[1].get() : nullptr;
  if (!b) throw SqlError(ErrKind::Syntax, "L2_DISTANCE takes exactly two arguments");
  if (a->kind == ExprKind::Column) {
    *col_out = a;
    return b;
  }
  *col_out = b;
  return a;
}

std::vector<float> literalVector(const Expr& e) {
  if (e.kind != ExprKind::Literal)
    throw SqlError(ErrKind::TypeMismatch, "L2_DISTANCE needs a vector literal");
  if (e.literal.type == DataType::Vector) return e.literal.v;
  if (e.literal.type == DataType::Varchar) return parseVectorText(e.literal.s);
  throw SqlError(ErrKind::TypeMismatch, "L2_DISTANCE needs a vector literal");
}

EvalResult evalGeneral(const Expr& e, const TableInfo& ti, const Row& row) {
  EvalResult out;
  switch (e.kind) {
    case ExprKind::Literal:
      out.val = e.literal;
      return out;
    case ExprKind::Column: {
      for (size_t i = 0; i < ti.columns.size(); ++i)
        if (ti.columns[i].name == e.column) {
          out.val = row[i];
          return out;
        }
      throw SqlError(ErrKind::NoSuchColumn, "column '" + e.column + "' is not in '" + ti.name + "'");
    }
    case ExprKind::Star:
      throw SqlError(ErrKind::Syntax, "* is only valid as a projection");
    case ExprKind::Call: {
      if (e.func != "L2_DISTANCE")
        throw SqlError(ErrKind::Unsupported, "function " + e.func + " is not supported");
      const Expr* col_e = nullptr;
      const Expr* val_e = vectorArgOf(e, &col_e);
      if (col_e->kind != ExprKind::Column)
        throw SqlError(ErrKind::Unsupported, "L2_DISTANCE needs a vector column");
      int idx = -1;
      for (size_t i = 0; i < ti.columns.size(); ++i)
        if (ti.columns[i].name == col_e->column) idx = static_cast<int>(i);
      if (idx < 0) throw SqlError(ErrKind::NoSuchColumn, "unknown column '" + col_e->column + "'");
      if (ti.columns[idx].type != DataType::Vector)
        throw SqlError(ErrKind::TypeMismatch, "L2_DISTANCE needs a VECTOR column");
      std::vector<float> query = literalVector(*val_e);
      if (static_cast<int64_t>(query.size()) != ti.columns[idx].vector_dim)
        throw SqlError(ErrKind::VectorDimMismatch, "query vector dimension does not match VECTOR(" +
                                                        std::to_string(ti.columns[idx].vector_dim) + ")");
      out.is_real = true;
      // The task defines L2_DISTANCE as the Euclidean distance, matching
      // seekdb/MySQL (which keeps the squared variant under a separate name).
      // Internally TopK still ranks on the squared value: sqrt is monotone,
      // so the order and the pk tie-break are unchanged while the emitted
      // number is the one the expected-output files compare against.
      out.real = std::sqrt(l2Squared(query.data(), row[idx].v.data(), query.size()));
      return out;
    }
    case ExprKind::Binary: {
      if (e.op == BinOp::And) {
        auto a = evalGeneral(*e.args[0], ti, row);
        if (!truthy(a.val)) {
          out.val = Value::bigint(0);
          return out;
        }
        auto b = evalGeneral(*e.args[1], ti, row);
        out.val = Value::bigint(truthy(b.val) ? 1 : 0);
        return out;
      }
      bool numeric = false;
      auto hasCall = [](const Expr& x) {
        return x.kind == ExprKind::Call && x.func == "L2_DISTANCE";
      };
      auto hasReal = [](const Expr& x) {
        return x.kind == ExprKind::Literal && x.literal.type == DataType::Real;
      };
      if (hasCall(*e.args[0]) || hasCall(*e.args[1]) || hasReal(*e.args[0]) || hasReal(*e.args[1]))
        numeric = true;
      if (numeric) {
        double x = evalReal(*e.args[0], ti, row);
        double y = evalReal(*e.args[1], ti, row);
        int c = x < y ? -1 : (x > y ? 1 : 0);
        bool ok = false;
        switch (e.op) {
          case BinOp::Eq: ok = c == 0; break;
          case BinOp::Ne: ok = c != 0; break;
          case BinOp::Lt: ok = c < 0; break;
          case BinOp::Le: ok = c <= 0; break;
          case BinOp::Gt: ok = c > 0; break;
          case BinOp::Ge: ok = c >= 0; break;
          case BinOp::And: ok = false; break;
        }
        out.val = Value::bigint(ok ? 1 : 0);
        return out;
      }
      auto a = evalGeneral(*e.args[0], ti, row);
      auto b = evalGeneral(*e.args[1], ti, row);
      int c = cmpValues(a.val, b.val);
      bool ok = false;
      switch (e.op) {
        case BinOp::Eq: ok = c == 0; break;
        case BinOp::Ne: ok = c != 0; break;
        case BinOp::Lt: ok = c < 0; break;
        case BinOp::Le: ok = c <= 0; break;
        case BinOp::Gt: ok = c > 0; break;
        case BinOp::Ge: ok = c >= 0; break;
        case BinOp::And: ok = truthy(a.val) && truthy(b.val); break;
      }
      out.val = Value::bigint(ok ? 1 : 0);
      return out;
    }
  }
  throw SqlError(ErrKind::Internal, "unreachable");
}

// Compiles a predicate into a closure over the raw record image. Every
// conjunct of the shape <column> <cmp> <literal> is answered straight from the
// page bytes; anything else falls back to decoding the row.
// Rejects unknown column names up front. Without this, a predicate that
// references a missing column silently returns an empty set whenever the
// table happens to be empty, which is exactly the "must not quietly mean
// something else" rule the task calls out.
void validatePredicateColumns(const TableInfo& ti, const Expr* e) {
  if (!e) return;
  switch (e->kind) {
    case ExprKind::Column: {
      for (const Column& c : ti.columns)
        if (c.name == e->column) return;
      throw SqlError(ErrKind::NoSuchColumn, "column '" + e->column + "' is not in '" + ti.name + "'");
    }
    case ExprKind::Call: {
      if (e->func != "L2_DISTANCE")
        throw SqlError(ErrKind::Unsupported, "function " + e->func + " is not supported");
      for (const auto& a : e->args) validatePredicateColumns(ti, a.get());
      return;
    }
    case ExprKind::Binary: {
      for (const auto& a : e->args) validatePredicateColumns(ti, a.get());
      return;
    }
    default:
      return;
  }
}

std::function<bool(const uint8_t*)> compilePredicate(const Table& t, const Expr* e) {
  if (!e) return [](const uint8_t*) { return true; };
  const TableInfo& ti = t.info();
  validatePredicateColumns(ti, e);

  if (e->kind == ExprKind::Binary && e->op == BinOp::And) {
    auto a = compilePredicate(t, e->args[0].get());
    auto b = compilePredicate(t, e->args[1].get());
    return [a, b](const uint8_t* rec) { return a(rec) && b(rec); };
  }

  if (e->kind == ExprKind::Binary) {
    const Expr* lhs = e->args[0].get();
    const Expr* rhs = e->args[1].get();
    const Expr* col = nullptr;
    const Expr* lit = nullptr;
    bool flipped = false;
    if (lhs->kind == ExprKind::Column) {
      col = lhs;
      lit = rhs;
    } else if (rhs->kind == ExprKind::Column) {
      col = rhs;
      lit = lhs;
      flipped = true;
    }
    if (col && lit && lit->kind == ExprKind::Literal) {
      int idx = -1;
      for (size_t i = 0; i < ti.columns.size(); ++i)
        if (ti.columns[i].name == col->column) idx = static_cast<int>(i);
      if (idx >= 0 && ti.columns[idx].type != DataType::Vector &&
          lit->literal.type == ti.columns[idx].type) {
        BinOp op = e->op;
        if (flipped) {
          if (op == BinOp::Lt) op = BinOp::Gt;
          else if (op == BinOp::Le) op = BinOp::Ge;
          else if (op == BinOp::Gt) op = BinOp::Lt;
          else if (op == BinOp::Ge) op = BinOp::Le;
        }
        size_t ci = static_cast<size_t>(idx);
        if (ti.columns[idx].type == DataType::Bigint) {
          int64_t w = lit->literal.i;
          return [ci, w, op](const uint8_t* rec) {
            int64_t x = recordBigint(rec, ci);
            switch (op) {
              case BinOp::Eq: return x == w;
              case BinOp::Ne: return x != w;
              case BinOp::Lt: return x < w;
              case BinOp::Le: return x <= w;
              case BinOp::Gt: return x > w;
              case BinOp::Ge: return x >= w;
              case BinOp::And: return false;
            }
            return false;
          };
        }
        std::string w = lit->literal.s;
        return [ci, w, op](const uint8_t* rec) {
          std::string_view x = recordVarchar(rec, ci);
          int c = 0;
          size_t n = std::min(x.size(), w.size());
          for (size_t i = 0; i < n; ++i) {
            unsigned char a = static_cast<unsigned char>(x[i]), b = static_cast<unsigned char>(w[i]);
            if (a != b) {
              c = a < b ? -1 : 1;
              break;
            }
          }
          if (c == 0) c = x.size() == w.size() ? 0 : (x.size() < w.size() ? -1 : 1);
          switch (op) {
            case BinOp::Eq: return c == 0;
            case BinOp::Ne: return c != 0;
            case BinOp::Lt: return c < 0;
            case BinOp::Le: return c <= 0;
            case BinOp::Gt: return c > 0;
            case BinOp::Ge: return c >= 0;
            case BinOp::And: return false;
          }
          return false;
        };
      }
    }
  }

  TableInfo copy = ti;
  const Expr* owned = e;
  return [copy, owned](const uint8_t* rec) {
    Row row;
    decodeRecord(rec, copy.columns, row);
    return truthy(evalGeneral(*owned, copy, row).val);
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// Coercion
// ---------------------------------------------------------------------------

Value Engine::coerce(const Expr& e, const Column& col, const TableInfo&) const {
  if (e.kind != ExprKind::Literal)
    throw SqlError(ErrKind::Unsupported, "only literals are accepted in this position");
  const Value& v = e.literal;
  if (v.type == DataType::Real)
    throw SqlError(ErrKind::TypeMismatch, "column '" + col.name + "' does not accept a real number");

  switch (col.type) {
    case DataType::Bigint:
      if (v.type != DataType::Bigint)
        throw SqlError(ErrKind::TypeMismatch, "column '" + col.name + "' requires BIGINT");
      return v;
    case DataType::Varchar:
      if (v.type != DataType::Varchar)
        throw SqlError(ErrKind::TypeMismatch, "column '" + col.name + "' requires VARCHAR");
      if (utf8Length(v.s) > static_cast<size_t>(col.varchar_len))
        throw SqlError(ErrKind::VarcharTooLong, "value for '" + col.name + "' exceeds VARCHAR(" +
                                                    std::to_string(col.varchar_len) + ")");
      return v;
    case DataType::Vector: {
      std::vector<float> f;
      if (v.type == DataType::Vector) f = v.v;
      else if (v.type == DataType::Varchar) f = parseVectorText(v.s);
      else throw SqlError(ErrKind::TypeMismatch, "column '" + col.name + "' requires VECTOR");
      if (static_cast<int64_t>(f.size()) != col.vector_dim)
        throw SqlError(ErrKind::VectorDimMismatch,
                       "vector for '" + col.name + "' has " + std::to_string(f.size()) +
                           " components, column is VECTOR(" + std::to_string(col.vector_dim) + ")");
      return Value::vector(std::move(f));
    }
  }
  throw SqlError(ErrKind::TypeMismatch, "bad column");
}

// ---------------------------------------------------------------------------
// Statement dispatch
// ---------------------------------------------------------------------------

void Engine::execute(const Statement& st, QueryResult& res) {
  switch (st.kind) {
    case StmtKind::Begin:
      beginTxn();
      return;
    case StmtKind::Commit:
      commitTxn();
      return;
    case StmtKind::Rollback:
      rollbackTxn();
      return;
    default:
      break;
  }

  beginStatement();
  try {
    switch (st.kind) {
      case StmtKind::CreateTable: doCreateTable(st.create_table); break;
      case StmtKind::DropTable: doDropTable(st.drop_table); break;
      case StmtKind::Insert: doInsert(st.insert, res); break;
      case StmtKind::Select: doSelect(st.select, res); break;
      case StmtKind::Update: doUpdate(st.update, res); break;
      case StmtKind::Delete: doDelete(st.delete_, res); break;
      case StmtKind::CreateVectorIndex: doCreateIndex(st.create_index); break;
      case StmtKind::Set: doSet(st.set); break;
      default: break;
    }
  } catch (...) {
    abortStatement();
    throw;
  }
  finishStatement();
}

void Engine::doCreateTable(const CreateTable& c) {
  if (table(c.name)) {
    if (c.if_not_exists) return;
    throw SqlError(ErrKind::DuplicateTable, "table '" + c.name + "' already exists");
  }
  TableInfo ti;
  ti.name = c.name;
  std::unordered_set<std::string> seen;
  int vectors = 0;
  for (const Column& src : c.columns) {
    if (!seen.insert(src.name).second)
      throw SqlError(ErrKind::Syntax, "duplicate column name '" + src.name + "'");
    if (src.type == DataType::Vector) ++vectors;
    ti.columns.push_back(src);
  }
  if (vectors > 1) throw SqlError(ErrKind::Unsupported, "at most one VECTOR column per table");
  if (ti.columns.empty()) throw SqlError(ErrKind::Syntax, "table needs at least one column");
  ti.pk_col = columnIndex(ti, c.pk_name);
  if (ti.pk_col < 0) throw SqlError(ErrKind::Syntax, "primary key column not found");
  if (ti.columns[static_cast<size_t>(ti.pk_col)].type != DataType::Bigint)
    throw SqlError(ErrKind::TypeMismatch, "primary key must be BIGINT");
  for (size_t i = 0; i < ti.columns.size(); ++i)
    if (ti.columns[i].type == DataType::Vector) ti.vector_col = static_cast<int>(i);

  Table probe(ti, pool_.get(), &cfg_);
  size_t need = probe.maxRecordSize() + sizeof(Slot) + sizeof(PageHeader);
  if (need > cfg_.page_size && tables_.empty()) {
    // Widening the page before any table exists keeps a high-dimensional
    // column storable without introducing per-file page sizes.
    size_t ps = cfg_.page_size;
    while (ps < need && ps <= 32768) ps *= 2;
    if (ps > 32768)
      throw SqlError(ErrKind::Unsupported, "the widest row does not fit the maximum page size");
    cfg_.page_size = ps;
    pool_ = std::make_unique<BufferPool>(cfg_.page_size, cfg_.pool_pages);
    wal_.close();
    wal_.open(joinPath(cfg_.data_dir, "mvd.wal"));
  } else if (need > cfg_.page_size) {
    throw SqlError(ErrKind::Unsupported,
                   "the widest row needs " + std::to_string(need) + " bytes but the page size is " +
                       std::to_string(cfg_.page_size) + " and tables already exist");
  }

  ti.oid = next_oid_++;
  auto t = std::make_unique<Table>(std::move(ti), pool_.get(), &cfg_);
  Table* raw = t.get();
  raw->setOid(raw->info().oid);
  raw->setEngine(this);
  // Register before touching any page: writing the header goes through the
  // undo path, which resolves the table by oid.
  by_oid_[raw->oid()] = raw;
  tables_[raw->info().name] = std::move(t);
  raw->openFiles(true);
  raw->readHeader();
  raw->writeHeaderToPage();
  pool_->flushFile(raw->heapFileId());
  writeCatalog();
}

void Engine::doDropTable(const DropTable& d) {
  auto it = tables_.find(d.name);
  if (it == tables_.end()) {
    if (d.if_exists) return;
    throw SqlError(ErrKind::NoSuchTable, "table '" + d.name + "' does not exist");
  }
  Table* t = it->second.get();
  stmt_overlay_.erase(t->oid());
  tx_overlay_.erase(t->oid());
  for (auto u = txn_dirty_.begin(); u != txn_dirty_.end();) {
    if (u->file == t->heapFileId()) {
      txn_dirty_set_.erase(*u);
      u = txn_dirty_.erase(u);
    } else {
      ++u;
    }
  }
  for (auto undo = stmt_undo_.begin(); undo != stmt_undo_.end();) {
    if (undo->first.file == t->heapFileId()) undo = stmt_undo_.erase(undo);
    else ++undo;
  }
  for (auto undo = tx_undo_.begin(); undo != tx_undo_.end();) {
    if (undo->first.file == t->heapFileId()) undo = tx_undo_.erase(undo);
    else ++undo;
  }
  t->dropFiles();
  by_oid_.erase(t->oid());
  tables_.erase(it);
  writeCatalog();
}

void Engine::doInsert(const Insert& ins, QueryResult& res) {
  Table* t = requireTable(ins.table);
  const TableInfo& ti = t->info();

  if (!ins.columns.empty() && ins.columns.size() != ti.columns.size())
    throw SqlError(ErrKind::Unsupported,
                   "INSERT must supply every column of '" + ti.name +
                       "'; omitted columns would have to become NULL, which this task excludes");

  Overlay& ov = *overlayFor(t->oid());
  const Overlay* to = txOverlay(t->oid());
  int64_t n = 0;
  for (const auto& row_exprs : ins.rows) {
    if (row_exprs.size() != ti.columns.size())
      throw SqlError(ErrKind::Syntax, "row has " + std::to_string(row_exprs.size()) +
                                          " values but '" + ti.name + "' has " +
                                          std::to_string(ti.columns.size()) + " columns");
    Row row(ti.columns.size());
    for (size_t i = 0; i < ti.columns.size(); ++i) row[i] = coerce(*row_exprs[i], ti.columns[i], ti);
    int64_t pk = row[static_cast<size_t>(ti.pk_col)].i;
    Rid existing;
    if (t->findPk(pk, existing, &ov, to))
      throw SqlError(ErrKind::DuplicateKey, "duplicate primary key " + std::to_string(pk));
    std::string err;
    if (!t->insertRow(row, ov, err)) throw SqlError(ErrKind::Unsupported, err);
    ++n;
  }
  res.affected = n;
}

namespace {

struct Projection {
  std::string name;
  int col = -1;              // direct column projection
  bool is_distance = false;
  std::vector<float> query;
  int vcol = -1;
};

}  // namespace

void Engine::doSelect(const Select& sel, QueryResult& res) {
  Table* t = requireTable(sel.table);
  const TableInfo& ti = t->info();
  const Overlay* so = stmtOverlay(t->oid());
  const Overlay* to = txOverlay(t->oid());

  std::vector<Projection> projs;
  for (size_t i = 0; i < sel.projections.size(); ++i) {
    const Expr* e = sel.projections[i].get();
    std::string alias = i < sel.aliases.size() ? sel.aliases[i] : std::string();
    if (!e || e->kind == ExprKind::Star) {
      for (size_t c = 0; c < ti.columns.size(); ++c) {
        Projection p;
        p.name = ti.columns[c].name;
        p.col = static_cast<int>(c);
        projs.push_back(std::move(p));
      }
      continue;
    }
    Projection p;
    if (e->kind == ExprKind::Column) {
      int idx = columnIndex(ti, e->column);
      if (idx < 0) throw SqlError(ErrKind::NoSuchColumn, "unknown column '" + e->column + "'");
      p.col = idx;
      p.name = alias.empty() ? e->column : alias;
    } else if (e->kind == ExprKind::Call && e->func == "L2_DISTANCE") {
      if (ti.vector_col < 0) throw SqlError(ErrKind::TypeMismatch, "no vector column in '" + ti.name + "'");
      const Expr* col_e = nullptr;
      const Expr* val_e = vectorArgOf(*e, &col_e);
      if (col_e->kind != ExprKind::Column)
        throw SqlError(ErrKind::Unsupported, "L2_DISTANCE needs a vector column");
      int idx = columnIndex(ti, col_e->column);
      if (idx < 0) throw SqlError(ErrKind::NoSuchColumn, "unknown column '" + col_e->column + "'");
      if (idx != ti.vector_col)
        throw SqlError(ErrKind::TypeMismatch, "L2_DISTANCE needs the table's vector column");
      p.query = literalVector(*val_e);
      if (static_cast<int64_t>(p.query.size()) != ti.columns[idx].vector_dim)
        throw SqlError(ErrKind::VectorDimMismatch,
                       "query vector has " + std::to_string(p.query.size()) + " components, column is VECTOR(" +
                           std::to_string(ti.columns[idx].vector_dim) + ")");
      p.is_distance = true;
      p.vcol = idx;
      p.name = alias.empty() ? "L2_DISTANCE" : alias;
    } else {
      throw SqlError(ErrKind::Unsupported, "this projection expression is not supported");
    }
    projs.push_back(std::move(p));
  }
  if (projs.empty()) throw SqlError(ErrKind::Syntax, "SELECT has no projections");

  res.columns.clear();
  for (const auto& p : projs) res.columns.push_back(p.name);

  auto pred = compilePredicate(*t, sel.where.get());

  int distance_proj = -1;
  for (size_t i = 0; i < projs.size(); ++i)
    if (projs[i].is_distance) { distance_proj = static_cast<int>(i); break; }

  // ORDER BY keys, resolved against projection names so `ORDER BY d` works
  // when d is the alias of a distance projection.
  struct SortKey {
    int proj = -1;
    int col = -1;  // table column, used when the sort key is not projected
    bool asc = true;
  };
  std::vector<SortKey> keys;
  for (const auto& term : sel.order_by) {
    SortKey k;
    k.asc = term.asc;
    const Expr* e = term.expr.get();
    if (!e || e->kind != ExprKind::Column)
      throw SqlError(ErrKind::Unsupported, "this ORDER BY expression is not supported");
    for (size_t i = 0; i < projs.size(); ++i)
      if (projs[i].name == e->column) k.proj = static_cast<int>(i);
    if (k.proj < 0) {
      int idx = columnIndex(ti, e->column);
      if (idx < 0) throw SqlError(ErrKind::NoSuchColumn, "unknown ORDER BY column '" + e->column + "'");
      for (size_t i = 0; i < projs.size(); ++i)
        if (projs[i].col == idx) k.proj = static_cast<int>(i);
      if (k.proj < 0) k.col = idx;
    }
    keys.push_back(k);
  }

  // ---- vector TopK -------------------------------------------------------
  if (distance_proj >= 0 && sel.limit >= 0) {
    bool asc = true;
    for (const auto& k : keys)
      if (k.proj == distance_proj) asc = k.asc;
    if (asc) {
      size_t want = static_cast<size_t>(sel.limit) + static_cast<size_t>(std::max<int64_t>(0, sel.offset));
      const std::vector<float>& q = projs[static_cast<size_t>(distance_proj)].query;
      std::vector<std::pair<Rid, double>> cands;
      // A scalar predicate must be evaluated against every record anyway, so
      // the approximate index cannot save the visit; exact scan it is.
      if (ann_mode_ && !sel.where) t->topKAnn(q.data(), q.size(), want, so, to, cands);
      else t->topKExact(q.data(), q.size(), want, pred, cands);
      if (sel.offset > 0) {
        if (static_cast<size_t>(sel.offset) >= cands.size()) cands.clear();
        else cands.erase(cands.begin(), cands.begin() + sel.offset);
      }
      res.has_rows = true;
      for (const auto& c : cands) {
        Row row;
        if (!t->readRecord(c.first, row)) continue;
        std::vector<Cell> out;
        out.reserve(projs.size());
        for (const auto& p : projs) {
          if (p.is_distance) out.push_back(Cell::ofReal(std::sqrt(c.second)));
          else out.push_back(Cell::of(row[static_cast<size_t>(p.col)]));
        }
        res.rows.push_back(std::move(out));
      }
      return;
    }
  }

  // ---- general path ------------------------------------------------------
  struct Item {
    Row row;
    std::vector<Cell> cells;
  };
  std::vector<Item> items;
  TableInfo copy_ti = ti;
  t->scan([&](const uint8_t* rec, Rid rid) {
    if (!pred(rec)) return true;
    Item it;
    decodeRecord(rec, copy_ti.columns, it.row);
    it.cells.reserve(projs.size());
    for (const auto& p : projs) {
      if (p.is_distance)
        it.cells.push_back(Cell::ofReal(
            std::sqrt(l2Squared(p.query.data(), it.row[static_cast<size_t>(p.vcol)].v.data(),
                                p.query.size()))));
      else
        it.cells.push_back(Cell::of(it.row[static_cast<size_t>(p.col)]));
    }
    items.push_back(std::move(it));
    return true;
  });

  if (!keys.empty()) {
    const size_t pkcol = static_cast<size_t>(ti.pk_col);
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
      for (const auto& k : keys) {
        int c = 0;
        if (k.proj >= 0) {
          const Cell& x = a.cells[static_cast<size_t>(k.proj)];
          const Cell& y = b.cells[static_cast<size_t>(k.proj)];
          if (x.kind == Cell::Int && y.kind == Cell::Int) c = x.i < y.i ? -1 : (x.i > y.i ? 1 : 0);
          else if (x.kind == Cell::Real && y.kind == Cell::Real) c = x.r < y.r ? -1 : (x.r > y.r ? 1 : 0);
          else if (x.kind == Cell::Str && y.kind == Cell::Str) c = utf8Compare(x.s, y.s);
          else throw SqlError(ErrKind::TypeMismatch, "ORDER BY over mixed types is not supported");
        } else {
          c = cmpValues(a.row[static_cast<size_t>(k.col)], b.row[static_cast<size_t>(k.col)]);
        }
        if (c != 0) return k.asc ? c < 0 : c > 0;
      }
      // The task fixes the tie-break for ordinary ORDER BY as well: equal
      // keys come out in ascending primary key order, not insertion order.
      int64_t pa = a.row[pkcol].i, pb = b.row[pkcol].i;
      return pa < pb;
    });
  }

  res.has_rows = true;
  size_t from = static_cast<size_t>(std::max<int64_t>(0, sel.offset));
  size_t limit = sel.limit >= 0 ? static_cast<size_t>(sel.limit) : items.size();
  for (size_t i = from; i < items.size() && i - from < limit; ++i)
    res.rows.push_back(std::move(items[i].cells));
}

void Engine::doUpdate(const Update& up, QueryResult& res) {
  Table* t = requireTable(up.table);
  const TableInfo& ti = t->info();
  Overlay& ov = *overlayFor(t->oid());
  const Overlay* to = txOverlay(t->oid());

  std::vector<size_t> set_idx;
  std::vector<Value> set_val;
  for (const auto& a : up.assignments) {
    int idx = columnIndex(ti, a.column);
    if (idx < 0) throw SqlError(ErrKind::NoSuchColumn, "unknown column '" + a.column + "'");
    if (idx == ti.pk_col)
      throw SqlError(ErrKind::Unsupported, "updating the primary key is not supported");
    set_idx.push_back(static_cast<size_t>(idx));
    set_val.push_back(coerce(*a.value, ti.columns[idx], ti));
  }

  auto pred = compilePredicate(*t, up.where.get());
  std::vector<std::pair<Rid, Row>> targets;
  t->scan([&](const uint8_t* rec, Rid rid) {
    if (!pred(rec)) return true;
    Row row;
    decodeRecord(rec, ti.columns, row);
    for (size_t k = 0; k < set_idx.size(); ++k) row[set_idx[k]] = set_val[k];
    targets.emplace_back(rid, std::move(row));
    return true;
  });

  int64_t n = 0;
  std::string err;
  for (auto& tr : targets) {
    int64_t pk = tr.second[static_cast<size_t>(ti.pk_col)].i;
    Rid cur_rid;
    // Resolve through the overlay: an earlier statement, or an earlier row of
    // this same statement, may already have moved the record.
    if (!t->findPk(pk, cur_rid, &ov, to)) continue;
    if (!t->rewriteRow(cur_rid, tr.second, ov, err)) throw SqlError(ErrKind::Unsupported, err);
    ++n;
  }
  res.affected = n;
}

void Engine::doDelete(const Delete& del, QueryResult& res) {
  Table* t = requireTable(del.table);
  const TableInfo& ti = t->info();
  Overlay& ov = *overlayFor(t->oid());
  const Overlay* to = txOverlay(t->oid());
  auto pred = compilePredicate(*t, del.where.get());

  std::vector<std::pair<Rid, int64_t>> targets;
  t->scan([&](const uint8_t* rec, Rid rid) {
    if (!pred(rec)) return true;
    targets.emplace_back(rid, recordBigint(rec, static_cast<size_t>(ti.pk_col)));
    return true;
  });
  int64_t n = 0;
  std::string err;
  for (const auto& tg : targets) {
    Rid live;
    if (!t->findPk(tg.second, live, &ov, to)) continue;  // already removed in this statement
    if (!t->deleteRow(live, tg.second, ov, err)) throw SqlError(ErrKind::Unsupported, err);
    ++n;
  }
  res.affected = n;
}

void Engine::doCreateIndex(const CreateVectorIndex& ci) {
  Table* t = requireTable(ci.table);
  const TableInfo& ti = t->info();
  if (ti.vector_col < 0)
    throw SqlError(ErrKind::TypeMismatch, "table '" + ti.name + "' has no vector column");
  if (columnIndex(ti, ci.column) != ti.vector_col)
    throw SqlError(ErrKind::Unsupported, "a vector index must cover the table's vector column");
  t->markIndexBuilt(true);
  t->rebuildIndexes();
  // The index now reflects the committed rows, so its epoch must match the
  // header for the sidecar to be considered valid after a restart.
  t->header().epoch += 1;
  t->writeHeaderToPage();
  pool_->flushFile(t->heapFileId());
  t->saveSidecars();
  writeCatalog();
}

void Engine::doSet(const SetStmt& s) {
  std::string k = s.key;
  for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::string v = s.value;
  if (k == "vector_mode" || k == "ann" || k == "mvd.ann" || k == "search_mode") {
    if (v == "ann" || v == "on" || v == "1" || v == "true") ann_mode_ = true;
    else if (v == "exact" || v == "off" || v == "0" || v == "false") ann_mode_ = false;
    else throw SqlError(ErrKind::Syntax, "unsupported value for " + k + ": " + v);
    return;
  }
  if (k == "autocommit") return;  // the driver manages BEGIN/COMMIT explicitly
  throw SqlError(ErrKind::Unsupported, "variable '" + k + "' is not recognised");
}

}  // namespace mvd
