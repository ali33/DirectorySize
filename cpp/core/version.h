// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Product identity strings shared by the apps.
#pragma once
#include <string>

#define DIRSIZE_VERSION "1.0.1"
#define DIRSIZE_PRODUCT "DirectorySize (C++)"
#define DIRSIZE_COPYRIGHT "Copyright © 2026 META.vn"
#define DIRSIZE_AUTHOR "Nguyễn Đức Sơn"
#define DIRSIZE_LICENSE "GNU GPL v2"
#define DIRSIZE_ORIGIN_URL "https://github.com/ali33/DirectorySize"

namespace dirsize {

// Multi-line text for `--version` and the GUI About box (UTF-8, '\n' newlines).
inline std::string about_text() {
  return std::string(DIRSIZE_PRODUCT) + " " + DIRSIZE_VERSION + "\n" + DIRSIZE_COPYRIGHT + "\n" +
         "Tác giả: " + DIRSIZE_AUTHOR + "\n" + "License: " + DIRSIZE_LICENSE + "\n" +
         "This program comes with ABSOLUTELY NO WARRANTY.\n" +
         "Viết lại bằng C++ từ DirectorySize (C#): " + DIRSIZE_ORIGIN_URL + "\n";
}

}  // namespace dirsize
