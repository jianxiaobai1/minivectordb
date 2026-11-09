#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "common/config.h"
#include "common/errors.h"
#include "engine.h"
#include "sql/ast.h"

namespace mvd {

namespace {

volatile sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

std::string renderFloat(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", d);
  return buf;
}

std::string renderVecComponent(float f) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(f));
  return buf;
}

std::string renderVector(const std::vector<float>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += renderVecComponent(v[i]);
  }
  out += "]";
  return out;
}

std::string renderInt(int64_t x) { return std::to_string(x); }

class Output {
 public:
  Output(const Config& cfg) : cfg_(cfg) {}

  void rows(const QueryResult& r) {
    if (!r.has_rows) return;
    if (cfg_.header) {
      std::string line;
      for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) line += cfg_.row_separator;
        line += r.columns[i];
      }
      emit(line);
    }
    for (const auto& row : r.rows) emit(renderRow(row));
  }

  std::string renderRow(const std::vector<Cell>& row) {
    std::string line;
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) line += cfg_.row_separator;
      const Cell& c = row[i];
      switch (c.kind) {
        case Cell::Int: line += renderInt(c.i); break;
        case Cell::Real: line += renderFloat(c.r); break;
        case Cell::Str: line += c.s; break;
        case Cell::Vec: line += renderVector(c.v); break;
      }
    }
    return line;
  }

  void status(const Statement& st, const QueryResult& r) {
    if (!cfg_.status_line) return;
    if (r.affected >= 0) {
      emit(cfg_.ok_prefix + " " + std::to_string(r.affected));
    } else if (!r.has_rows) {
      emit(cfg_.ok_prefix);
    }
  }

  void error(const SqlError& e) {
    if (cfg_.error_numeric_code) {
      emit(cfg_.error_prefix + " " + std::to_string(static_cast<int>(e.kind())) + ": " + e.what());
    } else {
      emit(cfg_.error_prefix + " " + ErrName(e.kind()) + ": " + e.what());
    }
  }

  void errorMessage(const std::string& msg) {
    emit(cfg_.error_prefix + " INTERNAL_ERROR: " + msg);
  }

  void ready() {
    if (!cfg_.ready_marker.empty()) emit(cfg_.ready_marker);
  }

  void emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
  }

  void flush() { std::fflush(stdout); }

 private:
  const Config& cfg_;
};

class Session {
 public:
  Session(Engine& engine, const Config& cfg) : engine_(engine), out_(cfg), cfg_(cfg) {}

  int run() {
    std::string line;
    while (!g_stop) {
      line.clear();
      if (!std::getline(std::cin, line)) {
        drain(true);
        break;
      }
      buffer_ += line;
      buffer_ += '\n';
      if (buffer_.size() > (1u << 20)) trim();
      drain(false);
    }
    return 0;
  }

 private:
  // Executes every complete statement currently in the buffer. A statement is
  // complete when it ends with `;`; a trailing fragment is only executed at
  // end of stream, so a statement split across lines is never run early.
  void drain(bool at_eof) {
    while (!g_stop) {
      Parser parser(buffer_, consumed_);
      std::unique_ptr<Statement> st;
      try {
        st = parser.parseStatement();
      } catch (const SqlError& e) {
        if (parser.incomplete() && !at_eof) return;  // wait for more input
        out_.error(e);
        out_.ready();
        out_.flush();
        skipToNextStatement();
        continue;
      } catch (const std::exception& e) {
        out_.errorMessage(e.what());
        out_.ready();
        out_.flush();
        skipToNextStatement();
        continue;
      }

      size_t end = parser.statementEnd();
      if (end <= consumed_ && !st) {
        if (end > consumed_) consumed_ = end;
        if (!at_eof) return;
        if (end == consumed_) return;  // nothing left
      }
      consumed_ = std::max(consumed_, end);
      if (!st) {
        if (at_eof && consumed_ >= buffer_.size()) return;
        continue;
      }

      QueryResult res;
      try {
        engine_.execute(*st, res);
        out_.rows(res);
        out_.status(*st, res);
      } catch (const SqlError& e) {
        out_.error(e);
      } catch (const std::exception& e) {
        out_.errorMessage(e.what());
      }
      out_.ready();
      out_.flush();
      trim();
    }
  }

  void skipToNextStatement() {
    // Drop the buffered text up to and including the next top-level `;` so a
    // bad statement cannot wedge the session.
    Lexer lex(buffer_, consumed_);
    while (true) {
      Token t = lex.next();
      if (t.kind == TokKind::Eof) {
        consumed_ = buffer_.size();
        return;
      }
      if (t.kind == TokKind::Punct && t.text == ";") {
        consumed_ = lex.offset();
        return;
      }
    }
  }

  void trim() {
    if (consumed_ == 0) return;
    if (consumed_ >= buffer_.size()) {
      buffer_.clear();
      consumed_ = 0;
      return;
    }
    if (buffer_.size() > 64 * 1024) {
      buffer_.erase(0, consumed_);
      consumed_ = 0;
    }
  }

  Engine& engine_;
  Output out_;
  const Config& cfg_;
  std::string buffer_;
  size_t consumed_ = 0;
};

int realMain(int argc, char** argv) {
  Config cfg;
  try {
    cfg = Config::fromArgs(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n", e.what());
    return 2;
  }

  std::unique_ptr<Engine> engine;
  try {
    engine = std::make_unique<Engine>(cfg);
    engine->startup();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }

  std::fprintf(stderr,
               "minivectordb ready data_dir=%s page_size=%zu pool_pages=%zu mode=%s "
               "direct_io=%zu startup_ms=%.2f recovery_ms=%.2f recovered_pages=%llu\n",
               cfg.data_dir.c_str(), cfg.page_size, cfg.pool_pages,
               cfg.vector_mode == Config::VectorMode::Ann ? "ann" : "exact",
               engine->pool().directFileCount(),
               engine->stats().startup_ms, engine->stats().recovery_ms,
               static_cast<unsigned long long>(engine->stats().recovered_pages));
  std::fflush(stderr);

  Session session(*engine, cfg);
  int rc = session.run();
  engine->shutdown();
  return rc;
}

}  // namespace

}  // namespace mvd

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGINT, mvd::onSignal);
  ::signal(SIGTERM, mvd::onSignal);
  return mvd::realMain(argc, argv);
}
