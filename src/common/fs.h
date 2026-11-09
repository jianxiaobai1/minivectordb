#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mvd {

std::string joinPath(const std::string& a, const std::string& b);
void mkdirp(const std::string& path);
bool writeFileAll(const std::string& path, const uint8_t* data, size_t n);
bool readFileAll(const std::string& path, std::vector<uint8_t>& out);
double nowMs();

}  // namespace mvd
