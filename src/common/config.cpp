#include "common/config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mvd {

static std::string envOr(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : fallback;
}

static bool startsWith(const std::string& s, const char* p) {
  return s.rfind(p, 0) == 0;
}

size_t Config::detectMemoryLimitBytes() {
  // cgroup v2 first: the benchmark measures memory.peak for the whole container
  // including page cache, so the pool must stay well below the hard limit.
  for (const char* path : {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"}) {
    std::ifstream in(path);
    if (!in) continue;
    std::string s;
    in >> s;
    if (s == "max" || s.empty()) continue;
    try {
      unsigned long long v = std::stoull(s);
      if (v > (1ull << 20) && v < (1ull << 48)) return static_cast<size_t>(v);
    } catch (...) {
    }
  }
  return 0;
}

Config Config::fromArgs(int argc, char** argv) {
  Config c;
  c.data_dir = envOr("MVD_DATA_DIR", "/data");
  c.result_format = envOr("MVD_RESULT_FORMAT", "tabs") == "csv"
                        ? ResultFormat::Csv
                        : (envOr("MVD_RESULT_FORMAT", "tabs") == "json" ? ResultFormat::Json
                                                                        : ResultFormat::Tabs);

  auto need = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + argv[i]);
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto inline_value = [&](const char* prefix, std::string& out) -> bool {
      if (!startsWith(a, prefix)) return false;
      out = a.substr(std::strlen(prefix));
      if (out.empty()) {
        if (i + 1 >= argc) throw std::runtime_error("missing value");
        out = argv[++i];
      }
      return true;
    };
    std::string val;
    if (inline_value("--data-dir=", val)) {
      c.data_dir = val;
    } else if (inline_value("--result-format=", val)) {
      c.result_format = val == "csv"      ? ResultFormat::Csv
                        : val == "json"   ? ResultFormat::Json
                                          : ResultFormat::Tabs;
    } else if (inline_value("--row-separator=", val)) {
      if (val == "\\t") val = "\t";
      c.row_separator = val;
    } else if (inline_value("--ready-marker=", val)) {
      c.ready_marker = val;
    } else if (inline_value("--error-prefix=", val)) {
      c.error_prefix = val;
    } else if (inline_value("--ok-prefix=", val)) {
      c.ok_prefix = val;
    } else if (inline_value("--vector-mode=", val)) {
      c.vector_mode = val == "ann" ? VectorMode::Ann : VectorMode::Exact;
    } else if (inline_value("--direct-io=", val)) {
      c.direct_io = val != "off" && val != "false" && val != "0";
    } else if (inline_value("--checkpoint-mb=", val)) {
      c.checkpoint_bytes = static_cast<size_t>(std::stoull(val)) << 20;
    } else if (inline_value("--pool-pages=", val)) {
      c.pool_pages = static_cast<size_t>(std::stoull(val));
    } else if (inline_value("--page-size=", val)) {
      c.page_size = static_cast<size_t>(std::stoull(val));
    } else if (inline_value("--ivf-nlist=", val)) {
      c.ivf_nlist = std::stoi(val);
    } else if (inline_value("--ivf-nprobe=", val)) {
      c.ivf_nprobe = std::stoi(val);
    } else if (a == "--header") {
      c.header = true;
    } else if (a == "--no-status") {
      c.status_line = false;
    } else if (a == "--numeric-errors") {
      c.error_numeric_code = true;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
                   "minivectordb [--data-dir=PATH] [--result-format=tabs|csv|json]\n"
                   "  [--row-separator=SEP] [--ready-marker=TOKEN] [--header] [--no-status]\n"
                   "  [--vector-mode=exact|ann] [--pool-pages=N] [--page-size=N]\n"
                   "  [--ivf-nlist=N] [--ivf-nprobe=N]\n");
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + a);
    }
  }
  if (c.pool_pages == 0) {
    size_t limit = detectMemoryLimitBytes();
    if (limit == 0) limit = 512ull << 20;
    // Leave headroom for the vector index, WAL buffers and libc overhead.
    size_t budget = limit > (64ull << 20) ? (limit * 3) / 5 : limit / 2;
    c.pool_pages = budget / c.page_size;
    if (c.pool_pages < 16) c.pool_pages = 16;
  }
  return c;
}

}  // namespace mvd
