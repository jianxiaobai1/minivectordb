#include "common/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mvd {

std::string joinPath(const std::string& a, const std::string& b) {
  if (!a.empty() && a.back() == '/') return a + b;
  return a + "/" + b;
}

void mkdirp(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] != '/' || cur.size() == 1) continue;
    if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST)
      throw std::runtime_error("mkdir " + cur + ": " + std::strerror(errno));
  }
  if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST)
    throw std::runtime_error("mkdir " + path + ": " + std::strerror(errno));
}

bool writeFileAll(const std::string& path, const uint8_t* data, size_t n) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, data + off, n - off);
    if (w <= 0) {
      ::close(fd);
      return false;
    }
    off += static_cast<size_t>(w);
  }
  ::fsync(fd);
  ::close(fd);
  return true;
}

bool readFileAll(const std::string& path, std::vector<uint8_t>& out) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return false;
  }
  out.resize(static_cast<size_t>(st.st_size));
  size_t off = 0;
  while (off < out.size()) {
    ssize_t r = ::read(fd, out.data() + off, out.size() - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  ::close(fd);
  return off == out.size();
}

double nowMs() {
  struct timeval tv;
  ::gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

}  // namespace mvd
