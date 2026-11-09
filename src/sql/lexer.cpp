#include "sql/lexer.h"

#include <algorithm>
#include <cctype>
#include "common/errors.h"

namespace mvd {

static bool isIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}
static bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

void Lexer::skipSpaceAndComments() {
  const std::string& s = *s_;
  while (p_ < s.size()) {
    char c = s[p_];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++p_;
      continue;
    }
    if (c == '-' && p_ + 1 < s.size() && s[p_ + 1] == '-') {
      while (p_ < s.size() && s[p_] != '\n') ++p_;
      continue;
    }
    if (c == '#') {
      while (p_ < s.size() && s[p_] != '\n') ++p_;
      continue;
    }
    if (c == '/' && p_ + 1 < s.size() && s[p_ + 1] == '*') {
      p_ += 2;
      while (p_ + 1 < s.size() && !(s[p_] == '*' && s[p_ + 1] == '/')) ++p_;
      if (p_ + 1 < s.size()) p_ += 2;
      continue;
    }
    return;
  }
}

Token Lexer::next() {
  skipSpaceAndComments();
  const std::string& s = *s_;
  Token t;
  t.pos = p_;
  if (p_ >= s.size()) {
    t.kind = TokKind::Eof;
    return t;
  }
  char c = s[p_];

  if (c == ';') {
    ++p_;
    t.kind = TokKind::Punct;
    t.text = ";";
    return t;
  }

  // Single-quoted string literal, with '' and backslash escapes.
  if (c == '\'') {
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= s.size()) throw SqlError(ErrKind::Syntax, "unterminated string literal");
      char d = s[p_];
      if (d == '\'') {
        if (p_ + 1 < s.size() && s[p_ + 1] == '\'') {
          out += '\'';
          p_ += 2;
          continue;
        }
        ++p_;
        break;
      }
      if (d == '\\' && p_ + 1 < s.size()) {
        char e = s[p_ + 1];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case '0': out += '\0'; break;
          case 'b': out += '\b'; break;
          case '\\': out += '\\'; break;
          case '\'': out += '\''; break;
          case '"': out += '"'; break;
          default: out += e; break;
        }
        p_ += 2;
        continue;
      }
      out += d;
      ++p_;
    }
    t.kind = TokKind::Str;
    t.text = std::move(out);
    return t;
  }

  // Quoted identifier: `name` or "name".
  if (c == '`' || c == '"') {
    char close = c;
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= s.size()) throw SqlError(ErrKind::Syntax, "unterminated quoted identifier");
      if (s[p_] == close) {
        if (p_ + 1 < s.size() && s[p_ + 1] == close) {
          out += close;
          p_ += 2;
          continue;
        }
        ++p_;
        break;
      }
      out += s[p_];
      ++p_;
    }
    t.kind = TokKind::Ident;
    t.text = std::move(out);  // case preserved for quoted identifiers
    return t;
  }

  // Number.
  if (std::isdigit(static_cast<unsigned char>(c)) ||
      (c == '.' && p_ + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[p_ + 1])))) {
    size_t start = p_;
    if (s[p_] == '-') ++p_;
    while (p_ < s.size() && std::isdigit(static_cast<unsigned char>(s[p_]))) ++p_;
    bool real = false;
    if (p_ < s.size() && s[p_] == '.') {
      if (p_ + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[p_ + 1]))) {
        real = true;
        ++p_;
        while (p_ < s.size() && std::isdigit(static_cast<unsigned char>(s[p_]))) ++p_;
      } else if (start == p_) {
        // lone '.' handled below as punctuation
      } else {
        real = true;
        ++p_;
      }
    }
    if (p_ < s.size() && (s[p_] == 'e' || s[p_] == 'E')) {
      size_t save = p_;
      ++p_;
      if (p_ < s.size() && (s[p_] == '+' || s[p_] == '-')) ++p_;
      if (p_ < s.size() && std::isdigit(static_cast<unsigned char>(s[p_]))) {
        real = true;
        while (p_ < s.size() && std::isdigit(static_cast<unsigned char>(s[p_]))) ++p_;
      } else {
        p_ = save;
      }
    }
    t.text = s.substr(start, p_ - start);
    t.kind = real ? TokKind::Real : TokKind::Int;
    return t;
  }

  if (isIdentStart(c)) {
    size_t start = p_;
    while (p_ < s.size() && isIdentChar(s[p_])) ++p_;
    t.kind = TokKind::Ident;
    std::string w = s.substr(start, p_ - start);
    for (auto& ch : w) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    t.text = std::move(w);
    return t;
  }

  // Two-character operators.
  if (p_ + 1 < s.size()) {
    std::string two = s.substr(p_, 2);
    if (two == "<=" || two == ">=" || two == "<>" || two == "!=" || two == "||") {
      p_ += 2;
      t.kind = TokKind::Punct;
      t.text = std::move(two);
      return t;
    }
  }

  p_ += 1;
  t.kind = TokKind::Punct;
  t.text = std::string(1, c);
  return t;
}

std::vector<uint32_t> utf8ToCodepoints(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
    else { cp = c; len = 1; }  // invalid byte: keep it, never crash
    if (i + len > s.size()) { cp = c; len = 1; }
    for (size_t k = 1; k < len; ++k) {
      unsigned char d = s[i + k];
      if ((d >> 6) != 0x2) { cp = c; len = 1; break; }
      cp = (cp << 6) | (d & 0x3F);
    }
    out.push_back(cp);
    i += len;
  }
  return out;
}

std::string utf8FromCodepoints(const std::vector<uint32_t>& cps) {
  std::string out;
  for (uint32_t cp : cps) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

// Code-point ordering, which for well-formed UTF-8 coincides with unsigned
// byte ordering.
int utf8Compare(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char x = a[i], y = b[i];
    if (x != y) return x < y ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

size_t utf8Length(const std::string& s) { return utf8ToCodepoints(s).size(); }

}  // namespace mvd
