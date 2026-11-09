#include "sql/ast.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "common/errors.h"

namespace mvd {

std::string Parser::identifier() {
  const Token& t = peek();
  if (t.kind != TokKind::Ident) fail("expected identifier");
  std::string name = t.text;
  advance();
  for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name;
}

int64_t Parser::integer() {
  const Token& t = peek();
  if (t.kind != TokKind::Int && t.kind != TokKind::Real) fail("expected integer");
  int64_t v = 0;
  try {
    size_t used = 0;
    long long raw = std::stoll(t.text, &used);
    if (used != t.text.size()) fail("expected integer");
    v = static_cast<int64_t>(raw);
  } catch (const SqlError&) {
    throw;
  } catch (...) {
    throw SqlError(ErrKind::ValueOutOfRange, "integer out of range: " + t.text);
  }
  advance();
  return v;
}

void Parser::fail(const std::string& msg) {
  // Running out of input mid-grammar means the statement is merely incomplete,
  // not invalid; the session loop then waits for more bytes instead of
  // reporting an error.
  if (peek().kind == TokKind::Eof) incomplete_ = true;
  throw SqlError(ErrKind::Syntax, msg + " near offset " + std::to_string(cur_.pos));
}

// A possibly signed numeric literal, used for vector components.
static double numberToken(const Token& t) {
  char* end = nullptr;
  double d = std::strtod(t.text.c_str(), &end);
  if (end != t.text.c_str() + t.text.size())
    throw SqlError(ErrKind::Syntax, "expected a number, got '" + t.text + "'");
  return d;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ExprPtr Parser::parseExpr() { return parseAnd(); }

ExprPtr Parser::parseAnd() {
  auto lhs = parseComparison();
  while (isKw("AND")) {
    advance();
    auto node = std::make_unique<Expr>();
    node->kind = ExprKind::Binary;
    node->op = BinOp::And;
    node->args.push_back(std::move(lhs));
    node->args.push_back(parseComparison());
    lhs = std::move(node);
  }
  return lhs;
}

static BinOp cmpOp(const std::string& p) {
  if (p == "=" || p == "==") return BinOp::Eq;
  if (p == "<") return BinOp::Lt;
  if (p == "<=") return BinOp::Le;
  if (p == ">") return BinOp::Gt;
  if (p == ">=") return BinOp::Ge;
  return BinOp::Ne;
}

ExprPtr Parser::parseComparison() {
  auto lhs = parsePrimary();
  const Token& t = peek();
  std::string p;
  if (t.kind == TokKind::Punct) p = t.text;
  BinOp op;
  bool is_cmp = true;
  if (p == "=" || p == "==" || p == "<" || p == "<=" || p == ">" || p == ">=") {
    op = cmpOp(p);
  } else if (p == "!=" || p == "<>") {
    op = BinOp::Ne;
  } else if (t.kind == TokKind::Ident) {
    if (t.text == "IS")
      throw SqlError(ErrKind::Unsupported, "IS / NULL predicates are not supported");
    if (t.text == "IN" || t.text == "LIKE" || t.text == "BETWEEN")
      throw SqlError(ErrKind::Unsupported, t.text + " predicate is not supported");
    if (t.text == "OR")
      throw SqlError(ErrKind::Unsupported, "OR is not supported by this task's SQL subset");
    else
      is_cmp = false;
  } else {
    is_cmp = false;
  }
  if (!is_cmp) return lhs;
  advance();
  auto node = std::make_unique<Expr>();
  node->kind = ExprKind::Binary;
  node->op = op;
  node->args.push_back(std::move(lhs));
  node->args.push_back(parsePrimary());
  return node;
}

Value Parser::parseVectorLiteral() {
  std::vector<float> out;
  expect("[");
  if (!at("]")) {
    while (true) {
      bool neg = false;
      if (at("-") || at("+")) {
        neg = peek().text == "-";
        advance();
      }
      const Token& t = peek();
      if (t.kind == TokKind::Ident) {
        // NaN / Inf spelled as identifiers are a malformed vector, not a
        // syntax slip: the task names INVALID_VECTOR for exactly this.
        std::string w = t.text;
        for (auto& ch : w) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (w == "nan" || w == "inf" || w == "infinity")
          throw SqlError(ErrKind::InvalidVector, "vector contains " + w);
      }
      if (t.kind != TokKind::Int && t.kind != TokKind::Real)
        fail("vector component must be a number");
      double d = numberToken(t);
      advance();
      if (neg) d = -d;
      if (d != d) throw SqlError(ErrKind::InvalidVector, "vector contains NaN");
      if (std::isinf(d) || d > std::numeric_limits<float>::max() ||
          d < -std::numeric_limits<float>::max())
        throw SqlError(ErrKind::InvalidVector, "vector component overflows float32");
      float f = static_cast<float>(d);
      if (std::isinf(f)) throw SqlError(ErrKind::InvalidVector, "vector contains Infinity");
      out.push_back(f);
      if (eat(",")) continue;
      break;
    }
  }
  expect("]");
  if (out.empty()) throw SqlError(ErrKind::VectorDimMismatch, "empty vector literal");
  return Value::vector(std::move(out));
}

ExprPtr Parser::parsePrimary() {
  const Token& t = peek();

  if (t.kind == TokKind::Punct && t.text == "(") {
    advance();
    auto e = parseExpr();
    expect(")");
    return e;
  }
  if (t.kind == TokKind::Punct && t.text == "[") {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->literal = parseVectorLiteral();
    return e;
  }
  if (t.kind == TokKind::Int) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->literal = Value::bigint(integer());
    return e;
  }
  if ((t.kind == TokKind::Punct && (t.text == "-" || t.text == "+")) || t.kind == TokKind::Real) {
    bool neg = t.kind == TokKind::Punct && t.text == "-";
    if (t.kind == TokKind::Punct) advance();
    const Token& n = peek();
    if (n.kind == TokKind::Int) {
      // The sign and the magnitude are parsed as one token: splitting them
      // makes -9223372036854775808 look like the out-of-range positive value
      // 9223372036854775808 and reject the legal BIGINT minimum.
      std::string txt = (neg ? "-" : "") + n.text;
      advance();
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(txt.c_str(), &end, 10);
      if (end != txt.c_str() + txt.size() || errno == ERANGE)
        throw SqlError(ErrKind::ValueOutOfRange, "integer out of range: " + txt);
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Literal;
      e->literal = Value::bigint(static_cast<int64_t>(v));
      return e;
    }
    if (n.kind == TokKind::Real) {
      std::string txt = (neg ? "-" : "") + n.text;
      advance();
      double d = std::strtod(txt.c_str(), nullptr);
      if (d != d || std::isinf(d))
        throw SqlError(ErrKind::ValueOutOfRange, "floating point literal is not finite: " + txt);
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Literal;
      e->literal = Value::real(d);
      return e;
    }
    fail("arithmetic expressions are not supported");
  }
  if (t.kind == TokKind::Str) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->literal = Value::varchar(t.text);
    advance();
    return e;
  }
  if (t.kind == TokKind::Punct && t.text == "*") {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Star;
    advance();
    return e;
  }
  if (t.kind == TokKind::Punct && t.text == "!") fail("unexpected token '!'");
  if (t.kind == TokKind::Punct && t.text == "NOT") fail("NOT is not supported");

  if (t.kind == TokKind::Ident) {
    const std::string up = t.text;
    if (up == "NULL") throw SqlError(ErrKind::Unsupported, "NULL is not supported");
    if (up == "NOT") throw SqlError(ErrKind::Unsupported, "NOT is not supported");
    if (up == "TRUE" || up == "FALSE") {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Literal;
      e->literal = Value::bigint(up == "TRUE" ? 1 : 0);
      advance();
      return e;
    }
    if (up == "OR") throw SqlError(ErrKind::Unsupported, "OR is not supported");

    // Look one token ahead without committing, to tell `col` from `f(...)` and
    // from the VECTOR[...] / ARRAY[...] constructors. The lexer has already
    // consumed the peeked identifier, so one next() yields the token after it.
    Token after = [&]() {
      Lexer c(lex_);
      return c.next();
    }();

    if ((up == "VECTOR" || up == "ARRAY") && after.kind == TokKind::Punct && after.text == "[") {
      advance();
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Literal;
      e->literal = parseVectorLiteral();
      return e;
    }
    if (after.kind == TokKind::Punct && after.text == "(") {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Call;
      e->func = up;
      advance();  // function name
      expect("(");
      if (!at(")")) {
        while (true) {
          e->args.push_back(parseExpr());
          if (eat(",")) continue;
          break;
        }
      }
      expect(")");
      return e;
    }

    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Column;
    std::string name = identifier();
    if (at(".")) {
      advance();
      if (at("*")) {
        advance();
        e->kind = ExprKind::Star;
        return e;
      }
      name = identifier();
    }
    e->column = name;
    return e;
  }

  fail("unexpected token '" + t.text + "'");
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static DataType parseTypeName(const std::string& up) {
  if (up == "BIGINT" || up == "INT" || up == "INTEGER" || up == "INT64" || up == "SMALLINT" ||
      up == "TINYINT")
    return DataType::Bigint;
  if (up == "VARCHAR" || up == "CHAR" || up == "TEXT" || up == "STRING") return DataType::Varchar;
  if (up == "VECTOR" || up == "VEC") return DataType::Vector;
  throw SqlError(ErrKind::Unsupported, "column type " + up + " is not supported");
}

static bool optionWord(const std::string& up) {
  static const char* kWords[] = {
      "ENGINE",        "CHARSET",     "CHARACTER",  "SET",        "COLLATE",
      "DEFAULT",       "ROW_FORMAT",  "COMMENT",    "COMPRESSION", "ENCRYPTION",
      "AUTO_INCREMENT", "KEY_BLOCK_SIZE", "CHECKSUM", "DELAY_KEY_WRITE",
      "PACK_KEYS",     "STATS_PERSISTENT", "TABLESPACE", "DATA", "INDEX"};
  for (const char* w : kWords)
    if (up == w) return true;
  return false;
}

// MySQL table options after `)` carry no semantics for this task and are
// skipped rather than rejected. Anything outside the option vocabulary stops
// the skip so the caller reports a precise error instead of swallowing SQL.
void Parser::skipTableOptions() {
  while (!at(";") && peek().kind != TokKind::Eof) {
    const Token& o = peek();
    if (o.kind == TokKind::Ident && optionWord(o.text)) {
      advance();
      continue;
    }
    if (at("=")) {
      advance();
      const Token& v = peek();
      if (v.kind == TokKind::Ident || v.kind == TokKind::Str || v.kind == TokKind::Int) {
        advance();
        continue;
      }
      fail("bad table option value");
    }
    if (at("(")) {  // ENGINE=InnoDB WITH ... style parentheses
      advance();
      while (!at(")") && peek().kind != TokKind::Eof) advance();
      expect(")");
      continue;
    }
    if (at(",")) {
      advance();
      continue;
    }
    break;
  }
}

std::unique_ptr<Statement> Parser::parseStatement() {
  incomplete_ = false;
  const Token& first = peek();
  if (first.kind == TokKind::Eof) {
    stmt_end_ = first.pos;
    return nullptr;
  }
  if (first.kind == TokKind::Punct && first.text == ";") {
    advance();
    stmt_end_ = cur_.pos + 1;
    return nullptr;
  }
  if (first.kind != TokKind::Ident) fail("expected a statement");

  auto finish = [&]() {
    if (at(";")) {
      advance();
      stmt_end_ = cur_.pos + 1;
    } else if (peek().kind == TokKind::Eof) {
      stmt_end_ = peek().pos;
    } else {
      fail("unexpected token after end of statement");
    }
    incomplete_ = false;
  };

  auto st = std::make_unique<Statement>();
  const std::string kw = first.text;

  if (kw == "CREATE") {
    advance();
    if (isKw("TABLE")) {
      advance();
      st->kind = StmtKind::CreateTable;
      if (eatKw("IF")) {
        expectKw("NOT");
        expectKw("EXISTS");
        st->create_table.if_not_exists = true;
      }
      st->create_table.name = identifier();
      expect("(");
      bool saw_pk = false;
      while (true) {
        const Token& e = peek();
        if (e.kind == TokKind::Ident && e.text == "PRIMARY") {
          advance();
          expectKw("KEY");
          expect("(");
          std::string pk = identifier();
          expect(")");
          if (saw_pk) throw SqlError(ErrKind::Syntax, "more than one primary key");
          st->create_table.pk_name = pk;
          saw_pk = true;
        } else if (e.kind == TokKind::Ident &&
                   (e.text == "UNIQUE" || e.text == "KEY" || e.text == "INDEX" ||
                    e.text == "CONSTRAINT" || e.text == "FOREIGN" || e.text == "FULLTEXT" ||
                    e.text == "SPATIAL" || e.text == "CHECK")) {
          throw SqlError(ErrKind::Unsupported, "table constraint " + e.text + " is not supported");
        } else {
          Column col;
          col.name = identifier();
          const Token& ty = peek();
          if (ty.kind != TokKind::Ident) fail("expected column type");
          DataType dt = parseTypeName(ty.text);
          advance();
          int64_t n = 0;
          if (at("(")) {
            advance();
            n = integer();
            while (eat(",")) {  // VARCHAR(n) BINARY / VECTOR(n, dist) style extras
              if (peek().kind == TokKind::Ident) advance();
              else integer();
            }
            expect(")");
          }
          col.type = dt;
          if (dt == DataType::Varchar) col.varchar_len = n > 0 ? n : 65535;
          if (dt == DataType::Vector) {
            if (n <= 0) fail("VECTOR requires a positive dimension");
            col.vector_dim = n;
          }
          while (peek().kind == TokKind::Ident) {
            const std::string m = peek().text;
            if (m == "PRIMARY") {
              advance();
              expectKw("KEY");
              if (saw_pk) throw SqlError(ErrKind::Syntax, "more than one primary key");
              if (dt != DataType::Bigint)
                throw SqlError(ErrKind::TypeMismatch, "primary key must be BIGINT");
              st->create_table.pk_name = col.name;
              saw_pk = true;
            } else if (m == "NOT") {
              advance();
              expectKw("NULL");
            } else if (m == "NULL") {
              advance();
            } else if (m == "UNIQUE" || m == "DEFAULT" || m == "AUTO_INCREMENT" || m == "COMMENT" ||
                       m == "UNSIGNED" || m == "COLLATE" || m == "CHARACTER") {
              throw SqlError(ErrKind::Unsupported, "column attribute " + m + " is not supported");
            } else {
              break;
            }
          }
          st->create_table.columns.push_back(std::move(col));
        }
        if (eat(",")) continue;
        break;
      }
      expect(")");
      if (st->create_table.pk_name.empty())
        throw SqlError(ErrKind::Syntax, "table requires exactly one BIGINT primary key");
      skipTableOptions();
      finish();
      return st;
    }
    if (isKw("VECTOR") || isKw("INDEX")) {
      eatKw("VECTOR");
      expectKw("INDEX");
      st->kind = StmtKind::CreateVectorIndex;
      if (eatKw("IF")) {
        expectKw("NOT");
        expectKw("EXISTS");
        st->create_index.if_not_exists = true;
      }
      st->create_index.index_name = identifier();
      expectKw("ON");
      st->create_index.table = identifier();
      expect("(");
      st->create_index.column = identifier();
      // Index options such as DISTANCE=L2, TYPE=HNSW, M=16 are accepted and
      // ignored: the engine selects its own algorithm.
      while (eat(",")) {
        while (peek().kind == TokKind::Ident) advance();
        if (at("=")) advance();
        const Token& v = peek();
        if (v.kind == TokKind::Ident || v.kind == TokKind::Int || v.kind == TokKind::Str) advance();
      }
      expect(")");
      finish();
      return st;
    }
    throw SqlError(ErrKind::Unsupported, "only CREATE TABLE and CREATE VECTOR INDEX are supported");
  }

  if (kw == "DROP") {
    advance();
    if (eatKw("TABLE")) {
      st->kind = StmtKind::DropTable;
      if (eatKw("IF")) {
        expectKw("EXISTS");
        st->drop_table.if_exists = true;
      }
      st->drop_table.name = identifier();
      finish();
      return st;
    }
    throw SqlError(ErrKind::Unsupported, "only DROP TABLE is supported");
  }

  if (kw == "INSERT") {
    advance();
    expectKw("INTO");
    st->kind = StmtKind::Insert;
    st->insert.table = identifier();
    if (at("(")) {
      advance();
      while (true) {
        st->insert.columns.push_back(identifier());
        if (eat(",")) continue;
        break;
      }
      expect(")");
    }
    if (isKw("ON"))
      throw SqlError(ErrKind::Unsupported, "INSERT ... ON DUPLICATE KEY is not supported");
    expectKw("VALUES");
    do {
      expect("(");
      std::vector<ExprPtr> row;
      if (!at(")")) {
        while (true) {
          row.push_back(parseExpr());
          if (eat(",")) continue;
          break;
        }
      }
      expect(")");
      st->insert.rows.push_back(std::move(row));
    } while (eat(","));
    finish();
    return st;
  }

  if (kw == "SELECT") {
    advance();
    st->kind = StmtKind::Select;
    if (eatKw("DISTINCT")) throw SqlError(ErrKind::Unsupported, "SELECT DISTINCT is not supported");
    eatKw("ALL");
    while (true) {
      st->select.projections.push_back(parseExpr());
      const Token& a = peek();
      if (a.kind == TokKind::Ident && a.text == "AS") {
        advance();
        st->select.aliases.push_back(identifier());
      } else if (a.kind == TokKind::Ident && a.text != "FROM" && a.text != "WHERE" &&
                 a.text != "ORDER" && a.text != "LIMIT" && a.text != "GROUP") {
        st->select.aliases.push_back(identifier());
      } else {
        st->select.aliases.emplace_back();
      }
      if (eat(",")) continue;
      break;
    }
    expectKw("FROM");
    st->select.table = identifier();
    {
      const Token& t = peek();
      if (t.kind == TokKind::Ident && t.text != "WHERE" && t.text != "ORDER" && t.text != "LIMIT" &&
          t.text != "GROUP" && t.text != "HAVING" && t.text != ";") {
        if (t.text == "FOR" || t.text == "LOCK")
          throw SqlError(ErrKind::Unsupported, "locking clauses are not supported");
        if (t.text == "UNION") throw SqlError(ErrKind::Unsupported, "UNION is not supported");
        if (t.text == "AS" || t.text == "USE" || t.text == "FORCE" || t.text == "JOIN" ||
            t.text == "STRAIGHT_JOIN" || t.text == "INNER" || t.text == "LEFT")
          throw SqlError(ErrKind::Unsupported, "table alias and JOIN are not supported");
        // A bare word here is an alias: MySQL allows `FROM t x`. This task
        // fixes one table per query, so accept and ignore it.
        identifier();
      }
    }
    if (eatKw("WHERE")) st->select.where = parseExpr();
    if (eatKw("GROUP")) throw SqlError(ErrKind::Unsupported, "GROUP BY is not supported");
    if (eatKw("HAVING")) throw SqlError(ErrKind::Unsupported, "HAVING is not supported");
    if (eatKw("ORDER")) {
      expectKw("BY");
      do {
        OrderTerm term;
        term.expr = parseExpr();
        if (eatKw("ASC")) term.asc = true;
        else if (eatKw("DESC")) term.asc = false;
        st->select.order_by.push_back(std::move(term));
      } while (eat(","));
    }
    if (eatKw("LIMIT")) {
      st->select.limit = integer();
      if (eat(",")) {
        st->select.offset = st->select.limit;
        st->select.limit = integer();
      } else if (eatKw("OFFSET")) {
        st->select.offset = integer();
      }
    }
    finish();
    return st;
  }

  if (kw == "UPDATE") {
    advance();
    st->kind = StmtKind::Update;
    st->update.table = identifier();
    expectKw("SET");
    do {
      Assignment asg;
      asg.column = identifier();
      expect("=");
      asg.value = parseExpr();
      st->update.assignments.push_back(std::move(asg));
    } while (eat(","));
    if (eatKw("WHERE")) st->update.where = parseExpr();
    finish();
    return st;
  }

  if (kw == "DELETE") {
    advance();
    expectKw("FROM");
    st->kind = StmtKind::Delete;
    st->delete_.table = identifier();
    if (eatKw("WHERE")) st->delete_.where = parseExpr();
    finish();
    return st;
  }

  if (kw == "BEGIN" || kw == "START") {
    advance();
    if (kw == "START") expectKw("TRANSACTION");
    eatKw("TRANSACTION");
    eatKw("WORK");
    if (eatKw("READ")) {
      if (peek().kind == TokKind::Ident && (peek().text == "ONLY" || peek().text == "WRITE")) advance();
    }
    st->kind = StmtKind::Begin;
    finish();
    return st;
  }
  if (kw == "COMMIT" || kw == "END") {
    advance();
    eatKw("WORK");
    st->kind = StmtKind::Commit;
    finish();
    return st;
  }
  if (kw == "ROLLBACK") {
    advance();
    if (isKw("TO")) throw SqlError(ErrKind::Unsupported, "savepoints are not supported");
    eatKw("TRANSACTION");
    eatKw("WORK");
    st->kind = StmtKind::Rollback;
    finish();
    return st;
  }
  if (kw == "SAVEPOINT" || kw == "RELEASE")
    throw SqlError(ErrKind::Unsupported, "nested transactions and savepoints are not supported");

  if (kw == "SET") {
    advance();
    eatKw("SESSION");
    eatKw("GLOBAL");
    st->kind = StmtKind::Set;
    st->set.key = identifier();
    while (at(".")) {
      advance();
      st->set.key += "." + identifier();
    }
    expect("=");
    const Token& v = peek();
    if (v.kind == TokKind::Str || v.kind == TokKind::Ident || v.kind == TokKind::Int) {
      st->set.value = v.text;
      advance();
    } else {
      fail("bad SET value");
    }
    for (auto& c : st->set.value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    finish();
    return st;
  }

  if (kw == "SHOW" || kw == "DESCRIBE" || kw == "DESC" || kw == "EXPLAIN" || kw == "USE" ||
      kw == "TRUNCATE" || kw == "ALTER" || kw == "ANALYZE" || kw == "OPTIMIZE" || kw == "FLUSH" ||
      kw == "LOAD" || kw == "CALL" || kw == "PREPARE" || kw == "EXECUTE" || kw == "DEALLOCATE" ||
      kw == "WITH" || kw == "REPLACE" || kw == "GRANT" || kw == "REVOKE")
    throw SqlError(ErrKind::Unsupported, kw + " is not supported by this task's SQL subset");

  throw SqlError(ErrKind::Unsupported, "statement starting with '" + kw + "' is not supported");
}

}  // namespace mvd
