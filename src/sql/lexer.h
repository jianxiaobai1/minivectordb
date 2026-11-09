#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mvd {

enum class TokKind {
  End,       // end of statement stream
  Eof,       // end of input
  Ident,     // keyword or identifier (upper-cased in `text`)
  Str,       // 'literal'
  Int,       // integer literal
  Real,      // floating point literal
  Punct,     // ( ) , ; = < <= >= > * . [ ]
};

struct Token {
  TokKind kind = TokKind::End;
  std::string text;
  long pos = 0;
};

class Lexer {
 public:
  // `input` must outlive the Lexer and must not be mutated while scanning;
  // callers pass the session buffer by reference so that one-token lookahead
  // can be taken by copying the Lexer cheaply.
  explicit Lexer(const std::string& input, size_t start = 0) : s_(&input), p_(start) {}

  // Scans the next token. `;` is emitted as a Punct token so the parser can
  // stop, and the caller can resume at `offset()` for the next statement.
  Token next();
  size_t offset() const { return p_; }
  const std::string& input() const { return *s_; }

 private:
  void skipSpaceAndComments();

  const std::string* s_;
  size_t p_ = 0;
};

// Parses a full UTF-8 string into code points; VARCHAR(n) length and string
// comparison are both defined over code points, not bytes.
std::vector<uint32_t> utf8ToCodepoints(const std::string& s);
std::string utf8FromCodepoints(const std::vector<uint32_t>& cps);
int utf8Compare(const std::string& a, const std::string& b);
size_t utf8Length(const std::string& s);

}  // namespace mvd
