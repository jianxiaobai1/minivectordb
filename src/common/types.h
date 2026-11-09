#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mvd {

// `Real` never appears in a table definition: it exists only as the type of a
// floating point literal and as the result of L2_DISTANCE.
enum class DataType : uint8_t { Bigint = 0, Varchar = 1, Vector = 2, Real = 3 };

struct Column {
  std::string name;
  DataType type = DataType::Bigint;
  int64_t varchar_len = 0;  // VARCHAR(n): max Unicode code points
  int64_t vector_dim = 0;   // VECTOR(n): dimensions
};

struct TableInfo {
  int64_t oid = 0;
  std::string name;
  std::vector<Column> columns;
  int pk_col = -1;       // index into columns; exactly one BIGINT primary key
  int vector_col = -1;   // index into columns; at most one vector column

  const Column* vectorColumn() const {
    return vector_col >= 0 ? &columns[vector_col] : nullptr;
  }
};

// A single SQL value. Vectors live in a contiguous float32 buffer so the
// distance kernel can run over them without per-element indirection.
struct Value {
  DataType type = DataType::Bigint;
  int64_t i = 0;
  double r = 0;
  std::string s;        // UTF-8 encoded for VARCHAR
  std::vector<float> v; // VECTOR payload

  static Value bigint(int64_t x) {
    Value r;
    r.type = DataType::Bigint;
    r.i = x;
    return r;
  }
  static Value real(double x) {
    Value o;
    o.type = DataType::Real;
    o.r = x;
    return o;
  }
  static Value varchar(std::string x) {
    Value r;
    r.type = DataType::Varchar;
    r.s = std::move(x);
    return r;
  }
  static Value vector(std::vector<float> x) {
    Value r;
    r.type = DataType::Vector;
    r.v = std::move(x);
    return r;
  }
};

using Row = std::vector<Value>;

// Physical record id: page number + slot index within the page.
struct Rid {
  uint32_t page = 0;
  uint16_t slot = 0;

  bool operator==(const Rid& o) const { return page == o.page && slot == o.slot; }
  bool operator!=(const Rid& o) const { return !(*this == o); }
  bool operator<(const Rid& o) const {
    return page != o.page ? page < o.page : slot < o.slot;
  }
};

struct RidHash {
  size_t operator()(const Rid& r) const {
    return (static_cast<size_t>(r.page) << 16) ^ r.slot;
  }
};

}  // namespace mvd
