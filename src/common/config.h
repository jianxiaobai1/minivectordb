#pragma once

#include <string>

namespace mvd {

// Everything the released test package is allowed to dictate about the wire
// protocol lives here and is settable from argv/env, so aligning with it does
// not require touching the engine.
struct Config {
  std::string data_dir = "/data";

  // Result framing.
  enum class ResultFormat { Tabs, Csv, Json } result_format = ResultFormat::Tabs;
  bool header = false;            // emit a column-name header line
  bool status_line = true;        // emit "OK <n>" after DML / DDL
  std::string row_separator = "\t";
  std::string ok_prefix = "OK";
  std::string error_prefix = "ERROR";
  bool error_numeric_code = false;

  // Readiness marker flushed after every statement response.
  std::string ready_marker;

  // Vector search mode selection (default exact, per task statement).
  enum class VectorMode { Exact, Ann } vector_mode = VectorMode::Exact;

  // Buffer pool sizing: 0 means auto-derive from the cgroup memory limit so a
  // dataset larger than the memory budget is paged rather than resident.
  size_t pool_pages = 0;
  size_t page_size = 4096;

  // Bypassing the page cache keeps the kernel's cached copy of the heap out of
  // the container's memory.peak, which the benchmark counts.
  bool direct_io = true;

  // Redo log size that triggers a checkpoint.
  size_t checkpoint_bytes = 64ull << 20;

  // IVF approximate index tuning.
  int ivf_nlist = 0;      // 0 = auto (~sqrt(n))
  int ivf_nprobe = 8;

  static Config fromArgs(int argc, char** argv);
  static size_t detectMemoryLimitBytes();
};

}  // namespace mvd
