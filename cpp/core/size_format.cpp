// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
#include "core/size_format.h"

#include <cstdio>

namespace dirsize {

static std::string scaled(double v, const char* unit) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  std::string s = buf;
  // Drop trailing zeros and a dangling point ("12.50" -> "12.5", "3.00" -> "3").
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  s += ' ';
  s += unit;
  return s;
}

std::string format_size(uint64_t b) {
  constexpr double KB = 1024.0, MB = KB * 1024, GB = MB * 1024, TB = GB * 1024;
  if (b == 0) return "0 Bytes";
  if (b < 1024) return std::to_string(b) + " Bytes";
  double d = static_cast<double>(b);
  if (d >= TB) return scaled(d / TB, "TB");
  if (d >= GB) return scaled(d / GB, "GB");
  if (d >= MB) return scaled(d / MB, "MB");
  return scaled(d / KB, "KB");
}

std::string format_count(uint64_t n) {
  std::string s = std::to_string(n), out;
  int c = 0;
  for (size_t i = s.size(); i-- > 0;) {
    out.insert(out.begin(), s[i]);
    if (++c % 3 == 0 && i != 0) out.insert(out.begin(), ',');
  }
  return out;
}

}  // namespace dirsize
