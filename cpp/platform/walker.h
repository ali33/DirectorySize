// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Platform seam. Core code only sees this interface; each OS provides
// make_default_walker() in its own .cpp (win_walker.cpp / posix_walker.cpp).
#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "core/size_tree.h"

namespace dirsize {

struct WalkOutcome {
  bool root_ok = false;     // false: root missing / not a directory / unreadable
  bool complete = true;     // false: the walk was cut short by the stop flag (result is partial)
  std::string root_error;   // UTF-8 message when !root_ok
};

// Windows only: which directory-listing API to use.
enum class ListApi { Default, Find, Nt };

struct WalkOptions {
  unsigned threads = 0;           // 0 = pick a sensible default
  bool one_file_system = false;   // POSIX: do not cross mount points (du -x)
  ListApi api = ListApi::Default; // Windows: FindFirstFileExW or NtQueryDirectoryFile
  // Count a file with several hard links only once. -1 = platform default
  // (POSIX: on, it is nearly free; Windows: off, it needs the file id of every entry).
  int dedup_hardlinks = -1;
  bool ignore_d_type = false;     // POSIX test hook: stat every entry as if d_type were DT_UNKNOWN
};

class IDirWalker {
 public:
  virtual ~IDirWalker() = default;

  // Validate/normalise the root and create the tree (display path + separator).
  // Returns nullptr and fills `error` (UTF-8) if the root is unusable.
  virtual std::unique_ptr<SizeTree> make_tree(const std::string& root_utf8, std::string& error) = 0;

  // Recursively list `root` (same string given to make_tree), feeding `tree`.
  // Must poll `stop` often and return soon after it becomes true.
  // Must not follow symlinks / junctions / reparse points.
  virtual WalkOutcome walk(const std::string& root_utf8, SizeTree& tree,
                           const std::atomic<bool>& stop) = 0;
};

std::unique_ptr<IDirWalker> make_default_walker(const WalkOptions& opt);

}  // namespace dirsize
