// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// OS-independent: human readable byte counts.
#pragma once
#include <cstdint>
#include <string>

namespace dirsize {

// Same scheme as the original C# app: "0 Bytes", "512 Bytes", "1.5 KB",
// "12.34 GB" (at most two decimals, trailing zeros dropped, '.' always the
// decimal separator). Adds a "TB" tier from 1024 GB up.
std::string format_size(uint64_t bytes);

// Thousands separated integer: 1234567 -> "1,234,567".
std::string format_count(uint64_t n);

}  // namespace dirsize
