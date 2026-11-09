#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "sql/lexer.h"

namespace mvd {

enum class ExprKind { Literal, Column, Call, Binary, Star };
enum class BinOp { Eq, Lt, Le, Gt, Ge, Ne, And };

struct Expr {
  ExprKind kind = ExprKind::Literal;
  Value literal;
  std::string column;         // Column
  std::string func;           // Call, upper-cased, e.g. "L2_DISTANCE"
  std::vector<std::unique_ptr<Expr>> args;
  BinOp op = BinOp::Eq;       // Binary
};

using ExprPtr = std::unique_ptr<Expr>;

enum class StmtKind {
  CreateTable,
  DropTable,
  Insert,
  Select,
  Update,
  Delete,
  Begin,
  Commit,
  Rollback,
  CreateVectorIndex,
  Set,
};

struct CreateTable {
  std::string name;
  std::vector<Column> columns;
  std::string pk_name;
  bool if_not_exists = false;
};

struct DropTable {
  std::string name;
  bool if_exists = false;
};

struct Insert {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<ExprPtr>> rows;
};

struct OrderTerm {
  ExprPtr expr;
  bool asc = true;
};

struct Select {
  std::string table;
  std::vector<ExprPtr> projections;  // empty means SELECT *
  std::vector<std::string> aliases;
  ExprPtr where;
  std::vector<OrderTerm> order_by;
  int64_t limit = -1;
  int64_t offset = 0;
};

struct Assignment {
  std::string column;
  ExprPtr value;
};

struct Update {
  std::string table;
  std::vector<Assignment> assignments;
  ExprPtr where;
};

struct Delete {
  std::string table;
  ExprPtr where;
};

struct CreateVectorIndex {
  std::string index_name;
  std::string table;
  std::string column;
  bool if_not_exists = false;
};

struct SetStmt {
  std::string key;
  std::string value;
};

struct Statement {
  StmtKind kind = StmtKind::Select;
  CreateTable create_table;
  DropTable drop_table;
  Insert insert;
  Select select;
  Update update;
  Delete delete_;
  CreateVectorIndex create_index;
  SetStmt set;
};

// Recursive-descent parser for the task's SQL subset.
//
// One token of lookahead. `statementEnd()` reports the offset just past the
// terminator of the last parsed statement, which is what the session loop uses
// to resume; the buffered lookahead token is deliberately ignored there.
class Parser {
 public:
  Parser(const std::string& input, size_t start) : lex_(input, start) {}

  // Returns nullptr for an empty statement. Throws SqlError(Syntax) with
  // incomplete() == true when more input is required.
  std::unique_ptr<Statement> parseStatement();
  size_t statementEnd() const { return stmt_end_; }
  bool incomplete() const { return incomplete_; }

 private:
  class Incomplete : public std::exception {};

  void fill() {
    if (!has_next_) {
      next_ = lex_.next();
      has_next_ = true;
    }
  }
  const Token& peek() {
    fill();
    return next_;
  }
  void advance() {
    fill();
    cur_ = next_;
    has_next_ = false;
    if (cur_.kind == TokKind::Eof) incomplete_ = true;
  }
  bool at(const char* punct) {
    const Token& t = peek();
    return t.kind == TokKind::Punct && t.text == punct;
  }
  bool eat(const char* punct) {
    if (at(punct)) {
      advance();
      return true;
    }
    return false;
  }
  bool isKw(const char* kw) {
    const Token& t = peek();
    return t.kind == TokKind::Ident && t.text == kw;
  }
  bool eatKw(const char* kw) {
    if (isKw(kw)) {
      advance();
      return true;
    }
    return false;
  }
  void expectKw(const char* kw) {
    if (!eatKw(kw)) fail(std::string("expected ") + kw);
  }
  void expect(const char* punct) {
    if (!eat(punct)) fail(std::string("expected '") + punct + "'");
  }
  std::string identifier();
  int64_t integer();
  [[noreturn]] void fail(const std::string& msg);

  ExprPtr parseExpr();
  ExprPtr parseAnd();
  ExprPtr parseComparison();
  ExprPtr parsePrimary();
  Value parseVectorLiteral();
  void skipTableOptions();

  Lexer lex_;
  Token cur_;
  Token next_;
  bool has_next_ = false;
  bool incomplete_ = false;
  size_t stmt_end_ = 0;
};

}  // namespace mvd
