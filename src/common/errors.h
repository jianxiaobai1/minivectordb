#pragma once

#include <stdexcept>
#include <string>

namespace mvd {

// Internal error taxonomy. The wire representation (prefix, numeric code) is
// produced by OutputFormat so that aligning with the released test package is
// a table change rather than an engine change.
enum class ErrKind {
  Syntax = 1,
  Unsupported = 2,
  NoSuchTable = 3,
  NoSuchColumn = 4,
  DuplicateTable = 5,
  DuplicateKey = 6,
  TypeMismatch = 7,
  ValueOutOfRange = 8,
  VarcharTooLong = 9,
  VectorDimMismatch = 10,
  InvalidVector = 11,
  TxnState = 12,
  Internal = 13,
  NotNull = 14,
};

class SqlError : public std::runtime_error {
 public:
  SqlError(ErrKind kind, std::string message)
      : std::runtime_error(message), kind_(kind) {}
  ErrKind kind() const { return kind_; }

 private:
  ErrKind kind_;
};

inline const char* ErrName(ErrKind k) {
  switch (k) {
    case ErrKind::Syntax: return "SYNTAX_ERROR";
    case ErrKind::Unsupported: return "UNSUPPORTED";
    case ErrKind::NoSuchTable: return "NO_SUCH_TABLE";
    case ErrKind::NoSuchColumn: return "NO_SUCH_COLUMN";
    case ErrKind::DuplicateTable: return "DUPLICATE_TABLE";
    case ErrKind::DuplicateKey: return "DUPLICATE_KEY";
    case ErrKind::TypeMismatch: return "TYPE_MISMATCH";
    case ErrKind::ValueOutOfRange: return "VALUE_OUT_OF_RANGE";
    case ErrKind::VarcharTooLong: return "VARCHAR_TOO_LONG";
    case ErrKind::VectorDimMismatch: return "VECTOR_DIM_MISMATCH";
    case ErrKind::InvalidVector: return "INVALID_VECTOR";
    case ErrKind::TxnState: return "TXN_STATE_ERROR";
    case ErrKind::Internal: return "INTERNAL_ERROR";
    case ErrKind::NotNull: return "NOT_NULL";
  }
  return "INTERNAL_ERROR";
}

}  // namespace mvd
