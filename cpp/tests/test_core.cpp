// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// OS-independent tests: size formatting and the size tree.
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "core/size_format.h"
#include "core/size_tree.h"

using namespace dirsize;

static void test_format() {
  CHECK_EQ(format_size(0), std::string("0 Bytes"));
  CHECK_EQ(format_size(1), std::string("1 Bytes"));
  CHECK_EQ(format_size(1023), std::string("1023 Bytes"));
  CHECK_EQ(format_size(1024), std::string("1 KB"));
  CHECK_EQ(format_size(1536), std::string("1.5 KB"));
  CHECK_EQ(format_size(1048576), std::string("1 MB"));
  CHECK_EQ(format_size(1073741824ull), std::string("1 GB"));
  CHECK_EQ(format_size(1073741824ull * 12 + 1073741824ull * 34 / 100), std::string("12.34 GB"));
  CHECK_EQ(format_size(1073741824ull * 5 / 2), std::string("2.5 GB"));
  CHECK_EQ(format_size(1099511627776ull), std::string("1 TB"));
  CHECK_EQ(format_count(0), std::string("0"));
  CHECK_EQ(format_count(999), std::string("999"));
  CHECK_EQ(format_count(1000), std::string("1,000"));
  CHECK_EQ(format_count(1234567), std::string("1,234,567"));
}

static void test_tree_basic() {
  SizeTree t("R:/root", '/');
  auto a = t.add_dir(SizeTree::kRoot, "a");
  auto b = t.add_dir(SizeTree::kRoot, "b");
  auto a1 = t.add_dir(a, "Thư mục Việt");
  t.add_files(SizeTree::kRoot, 10, 1);
  t.add_files(a, 100, 2);
  t.add_files(a1, 1000, 3);
  t.add_files(b, 50, 1);
  t.note_inaccessible("R:/root/x", "denied");
  t.note_skipped_link();

  SizeSnapshot s = t.snapshot(10);
  CHECK_EQ(s.dirs, uint64_t{4});
  CHECK_EQ(s.files, uint64_t{7});
  CHECK_EQ(s.total_bytes, uint64_t{1160});
  CHECK_EQ(s.inaccessible, uint64_t{1});
  CHECK_EQ(s.skipped_links, uint64_t{1});
  CHECK_EQ(s.error_samples.size(), size_t{1});
  CHECK_EQ(s.top.size(), size_t{4});
  CHECK_EQ(s.top[0].path, std::string("R:/root"));
  CHECK_EQ(s.top[0].bytes, uint64_t{1160});
  CHECK_EQ(s.top[1].path, std::string("R:/root/a"));
  CHECK_EQ(s.top[1].bytes, uint64_t{1100});
  CHECK_EQ(s.top[2].path, std::string("R:/root/a/Thư mục Việt"));
  CHECK_EQ(s.top[2].bytes, uint64_t{1000});
  CHECK_EQ(s.top[3].path, std::string("R:/root/b"));
  CHECK_EQ(s.top[3].bytes, uint64_t{50});

  SizeSnapshot s2 = t.snapshot(2);  // top-N limit
  CHECK_EQ(s2.top.size(), size_t{2});
  CHECK_EQ(s2.top[1].path, std::string("R:/root/a"));
  CHECK_EQ(t.snapshot(0).top.size(), size_t{0});
}

static void test_tree_drive_root_join() {
  SizeTree t("C:\\", '\\');  // root already ends with a separator: no doubling
  auto d = t.add_dir(SizeTree::kRoot, "Windows");
  t.add_files(d, 5, 1);
  SizeSnapshot s = t.snapshot(5);
  CHECK_EQ(s.top[1].path, std::string("C:\\Windows"));
}

// Many threads create dirs and add files while another thread snapshots.
static void test_tree_concurrent() {
  SizeTree t("/r", '/');
  constexpr int kThreads = 8, kDirs = 20000;
  std::vector<std::thread> th;
  std::atomic<bool> done{false};
  std::thread snap([&] {
    while (!done.load()) {
      SizeSnapshot s = t.snapshot(5);
      CHECK(s.total_bytes <= uint64_t{kThreads} * kDirs * 3);
    }
  });
  for (int i = 0; i < kThreads; ++i)
    th.emplace_back([&] {
      SizeTree::NodeId parent = SizeTree::kRoot;
      for (int k = 0; k < kDirs; ++k) {
        auto id = t.add_dir(k % 50 == 0 ? SizeTree::kRoot : parent, "d");
        t.add_files(id, 3, 1);
        parent = id;
      }
    });
  for (auto& x : th) x.join();
  done = true;
  snap.join();
  SizeSnapshot s = t.snapshot(1);
  CHECK_EQ(s.total_bytes, uint64_t{kThreads} * kDirs * 3);
  CHECK_EQ(s.files, uint64_t{kThreads} * kDirs);
  CHECK_EQ(s.dirs, uint64_t{kThreads} * kDirs + 1);
}

// While workers keep writing, successive snapshots must never go backwards (totals,
// files, directories), the parent never drops below a child, and the final snapshot
// after the writers stop must be exact (the snapshot cache must not serve stale data).
static void test_snapshot_monotonic() {
  SizeTree t("/r", '/');
  constexpr int kThreads = 6, kDirs = 6000;
  std::atomic<bool> done{false};
  std::atomic<int> violations{0};
  std::thread snap([&] {
    uint64_t last_total = 0, last_files = 0, last_dirs = 0;
    while (!done.load()) {
      SizeSnapshot s = t.snapshot(3);
      if (s.total_bytes < last_total || s.files < last_files || s.dirs < last_dirs) ++violations;
      for (const auto& e : s.top)
        if (e.bytes > s.total_bytes) ++violations;  // a child larger than the root
      if (!s.top.empty() && s.top[0].bytes != s.total_bytes) ++violations;  // root is always the largest
      last_total = s.total_bytes;
      last_files = s.files;
      last_dirs = s.dirs;
    }
  });
  std::vector<std::thread> th;
  for (int i = 0; i < kThreads; ++i)
    th.emplace_back([&] {
      SizeTree::NodeId parent = SizeTree::kRoot;
      for (int k = 0; k < kDirs; ++k) {
        auto id = t.add_dir(k % 40 == 0 ? SizeTree::kRoot : parent, "d");
        t.add_files(id, 2, 1);
        parent = id;
      }
    });
  for (auto& x : th) x.join();
  done = true;
  snap.join();
  CHECK_EQ(violations.load(), 0);
  SizeSnapshot a = t.snapshot(3);
  CHECK_EQ(a.total_bytes, uint64_t{kThreads} * kDirs * 2);
  CHECK_EQ(a.files, uint64_t{kThreads} * kDirs);
  CHECK_EQ(a.dirs, uint64_t{kThreads} * kDirs + 1);
  // Cache: unchanged tree gives the same answer; any change invalidates it.
  SizeSnapshot b = t.snapshot(3);
  CHECK_EQ(b.total_bytes, a.total_bytes);
  t.add_files(SizeTree::kRoot, 5, 1);
  CHECK_EQ(t.snapshot(3).total_bytes, a.total_bytes + 5);
  auto n = t.add_dir(SizeTree::kRoot, "late");
  CHECK_EQ(t.snapshot(3).dirs, a.dirs + 1);
  t.add_files(n, 7, 1);
  CHECK_EQ(t.snapshot(3).total_bytes, a.total_bytes + 12);
  CHECK_EQ(t.snapshot(1).top.size(), size_t{1});  // different top_n is not served from the cache
}

int main() {
  test_format();
  test_snapshot_monotonic();
  test_tree_basic();
  test_tree_drive_root_join();
  test_tree_concurrent();
  return finish("test_core");
}
