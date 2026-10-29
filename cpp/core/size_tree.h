// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// OS-independent: thread-safe accumulation of per-directory sizes.
//
// Design (chosen for scan speed):
//  * One node per directory in a chunked arena. Ids are handed out with an
//    atomic counter, so add_dir() takes no lock; the parent id is always
//    smaller than the child id.
//  * A worker adds the files of the directory it is scanning to that node's
//    OWN size only (one atomic add per directory, not one per file and not
//    one per ancestor).
//  * Recursive totals are computed only when a snapshot is taken, with one
//    reverse pass over the nodes (children before parents), O(directories).
//  * Full paths are never stored; they are rebuilt from (parent, name) for the
//    few top-N entries that get displayed.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dirsize {

struct DirEntryInfo {
  std::string path;  // UTF-8, display form (never a \\?\ path)
  uint64_t bytes = 0;
};

struct SizeSnapshot {
  std::vector<DirEntryInfo> top;  // largest first, at most top_n entries
  uint64_t dirs = 0;              // directories discovered so far (incl. root)
  uint64_t files = 0;
  uint64_t total_bytes = 0;       // recursive size of the root
  uint64_t inaccessible = 0;      // directories that could not be listed
  uint64_t skipped_links = 0;     // links / other filesystems not followed
  std::vector<std::string> error_samples;  // first few "path: reason" lines
};

class SizeTree {
 public:
  using NodeId = uint32_t;
  static constexpr NodeId kRoot = 0;

  // root_display: path as shown to the user. sep: path separator used when
  // rebuilding child paths ('\\' on Windows, '/' on POSIX).
  SizeTree(std::string root_display, char sep);
  ~SizeTree();
  SizeTree(const SizeTree&) = delete;
  SizeTree& operator=(const SizeTree&) = delete;

  // Register a sub-directory of `parent`; returns its id. `name` is a single
  // path component (UTF-8). Lock-free except when a new arena chunk is needed.
  NodeId add_dir(NodeId parent, const std::string& name);

  // `bytes` of files (count `file_count`) found directly inside `dir`.
  void add_files(NodeId dir, uint64_t bytes, uint64_t file_count);

  // Record a directory that could not be read (counted and sampled, never
  // swallowed, so UIs can show it).
  void note_inaccessible(const std::string& display_path, const std::string& reason);
  void note_skipped_link() { skipped_links_.fetch_add(1, std::memory_order_relaxed); }

  // Cheap enough to call every few hundred ms while workers keep running.
  SizeSnapshot snapshot(size_t top_n) const;

 private:
  struct Node {
    NodeId parent = 0;
    std::string name;
    std::atomic<uint64_t> own{0};
    std::atomic<bool> ready{false};  // fields above are published
  };
  static constexpr size_t kChunkBits = 16;
  static constexpr size_t kChunkSize = size_t{1} << kChunkBits;
  static constexpr size_t kMaxChunks = 4096;  // 268M directories

  Node& node(NodeId id) const {
    return chunks_[id >> kChunkBits].load(std::memory_order_acquire)[id & (kChunkSize - 1)];
  }
  void ensure_chunk(size_t c);

  std::string root_display_;
  char sep_;
  mutable std::array<std::atomic<Node*>, kMaxChunks> chunks_;
  std::mutex chunk_mu_;
  std::atomic<uint32_t> count_{0};
  std::atomic<uint64_t> files_{0};
  std::atomic<uint64_t> inaccessible_{0};
  std::atomic<uint64_t> skipped_links_{0};
  mutable std::mutex err_mu_;
  std::vector<std::string> error_samples_;
};

}  // namespace dirsize
