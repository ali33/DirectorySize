// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Walker integration tests: build a real temp tree with known sizes (including
// non-ASCII names), scan it with 1..N threads and every listing API, compare
// every directory total, then check links, stop flag and bad roots.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define GETPID getpid
#endif

#include "check.h"
#include "core/scanner.h"
#include "platform/walker.h"

namespace fs = std::filesystem;
using namespace dirsize;

static void write_file(const fs::path& p, size_t n) {
  std::ofstream f(p, std::ios::binary);
  std::string data(n, 'x');
  f.write(data.data(), static_cast<std::streamsize>(n));
}

static fs::path P(const std::string& u8) { return fs::u8path(u8); }

// Builds the tree and fills expected: relative-path -> recursive total (only >0).
struct Fixture {
  fs::path root;
  std::map<std::string, uint64_t> expected;  // key: "" for root, else "/a/b" (always '/')
  uint64_t files = 0, dirs = 1;

  uint64_t make(const fs::path& dir, const std::string& rel, int depth) {
    uint64_t total = 0;
    int nfiles = (depth * 7 + 3) % 5;  // 0..4 files
    for (int i = 0; i < nfiles; ++i) {
      size_t sz = static_cast<size_t>(depth * 1000 + i * 37 + 1);
      write_file(dir / P("tệp_" + std::to_string(i) + ".bin"), sz);
      total += sz;
      ++files;
    }
    if (depth < 3) {
      const char* names[] = {"alpha", "Thư mục Việt", "日本語", "with space"};
      int nsub = depth == 0 ? 4 : 3;
      for (int i = 0; i < nsub; ++i) {
        std::string nm = names[i];
        fs::create_directory(dir / P(nm));
        ++dirs;
        total += make(dir / P(nm), rel + "/" + nm, depth + 1);
      }
    }
    if (total > 0) expected[rel] = total;
    return total;
  }
};

static std::string sep_to_slash(std::string s) {
  for (auto& c : s)
    if (c == '\\') c = '/';
  return s;
}

// Compare a full snapshot against the fixture.
static void compare(const SizeSnapshot& s, const Fixture& fx, const char* label) {
  CHECK_EQ(s.top.empty(), false);
  if (s.top.empty()) return;
  std::string root = sep_to_slash(s.top[0].path);
  for (const auto& e : s.top)
    if (sep_to_slash(e.path).size() < root.size()) root = sep_to_slash(e.path);
  std::map<std::string, uint64_t> got;
  for (const auto& e : s.top) got[sep_to_slash(e.path).substr(root.size())] = e.bytes;
  if (got != fx.expected) {
    std::fprintf(stderr, "  mismatch in %s: expected %zu dirs, got %zu\n", label, fx.expected.size(), got.size());
    for (auto& kv : fx.expected) {
      auto it = got.find(kv.first);
      if (it == got.end() || it->second != kv.second) {
        std::fprintf(stderr, "    %s: expected %llu got %llu\n", kv.first.c_str(),
                     static_cast<unsigned long long>(kv.second),
                     static_cast<unsigned long long>(it == got.end() ? 0 : it->second));
        break;
      }
    }
  }
  CHECK(got == fx.expected);
  CHECK_EQ(s.total_bytes, fx.expected.at(""));
  CHECK_EQ(s.files, fx.files);
  CHECK_EQ(s.dirs, fx.dirs);
  CHECK_EQ(s.inaccessible, uint64_t{0});
}

static SizeSnapshot run_walker(const std::string& root, const WalkOptions& o, bool& ok) {
  auto w = make_default_walker(o);
  std::string err;
  auto tree = w->make_tree(root, err);
  ok = tree != nullptr;
  if (!tree) return {};
  std::atomic<bool> stop{false};
  WalkOutcome out = w->walk(root, *tree, stop);
  ok = out.root_ok;
  return tree->snapshot(1000000);
}

static bool make_dir_link(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
  // Wide command line: the paths contain non-ANSI characters.
  std::wstring cmd = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>nul";
  return _wsystem(cmd.c_str()) == 0;
#else
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  return !ec;
#endif
}

int main() {
  fs::path base = fs::temp_directory_path() / P("dirsize_test_" + std::to_string(GETPID()) + "_Việt");
  fs::remove_all(base);
  fs::create_directories(base);
  Fixture fx;
  fx.root = base / "tree";
  fs::create_directory(fx.root);
  fx.make(fx.root, "", 0);
  std::printf("fixture: %llu dirs, %llu files, %llu bytes\n", (unsigned long long)fx.dirs,
              (unsigned long long)fx.files, (unsigned long long)fx.expected[""]);

  std::string root = fx.root.u8string();

  // 1) Every thread count x every listing API, repeated to shake out races.
  std::vector<ListApi> apis = {ListApi::Default};
#ifdef _WIN32
  apis = {ListApi::Find, ListApi::Nt};
#endif
  for (ListApi api : apis)
    for (unsigned threads : {1u, 2u, 4u, 8u, 16u})
      for (int rep = 0; rep < 15; ++rep) {
        WalkOptions o;
        o.threads = threads;
        o.api = api;
        bool ok = false;
        SizeSnapshot s = run_walker(root, o, ok);
        CHECK(ok);
        compare(s, fx, "plain scan");
      }

  // 2) Trailing separator on the root must not change the result.
  {
    WalkOptions o;
    o.threads = 4;
    bool ok = false;
    SizeSnapshot s = run_walker(root + (std::string(1, static_cast<char>(fs::path::preferred_separator))), o, ok);
    CHECK(ok);
    compare(s, fx, "trailing separator");
  }

  // 3) Links/junctions are not followed (no double counting, no endless loop).
  {
    fs::path other = base / "other";
    fs::create_directory(other);
    write_file(other / "big.bin", 5000);
    bool linked = make_dir_link(fx.root / "link_to_other", other);
    bool loop = make_dir_link(fx.root / "alpha" / "loop_to_root", fx.root);
    if (linked && loop) {
#ifndef _WIN32
      // POSIX counts a symlink's own (lstat) size as a file, like du --apparent-size.
      for (auto& rel : {std::string("link_to_other"), std::string("alpha/loop_to_root")}) {
        struct stat st;
        lstat((fx.root / P(rel)).c_str(), &st);
        std::string parent = rel.find('/') == std::string::npos ? "" : "/alpha";
        fx.expected[""] += static_cast<uint64_t>(st.st_size);
        if (!parent.empty()) fx.expected[parent] += static_cast<uint64_t>(st.st_size);
        ++fx.files;
      }
#endif
      WalkOptions o;
      o.threads = 4;
      bool ok = false;
      SizeSnapshot s = run_walker(root, o, ok);
      CHECK(ok);
      compare(s, fx, "with links");  // same totals as before: links ignored
      CHECK(s.skipped_links >= 2);
    } else {
#ifdef _WIN32
      CHECK(linked && loop);  // junctions need no privilege: failing to create one is a test failure
#else
      std::printf("note: could not create directory links, link test skipped\n");
#endif
    }
  }

  // 4) Stop flag set before the walk: returns promptly, no crash.
  {
    WalkOptions o;
    o.threads = 4;
    auto w = make_default_walker(o);
    std::string err;
    auto tree = w->make_tree(root, err);
    std::atomic<bool> stop{true};
    auto t0 = std::chrono::steady_clock::now();
    WalkOutcome out = w->walk(root, *tree, stop);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(out.root_ok);
    CHECK(ms < 2000);
  }

  // 5) Bad roots are reported, not swallowed.
  {
    auto w = make_default_walker(WalkOptions{});
    std::string err;
    CHECK(w->make_tree((base / "does_not_exist").u8string(), err) == nullptr);
    CHECK(!err.empty());
    err.clear();
    CHECK(w->make_tree((fx.root / "tệp_0.bin").u8string(), err) == nullptr);
    err.clear();
    CHECK(w->make_tree("", err) == nullptr);
  }

  // 6) Scanner end to end: finishes, callback fires, state is Finished.
  {
    std::atomic<int> cb{0};
    Scanner sc(make_default_walker(WalkOptions{}));
    std::string err;
    CHECK(sc.start(root, [&](ScanState) { cb++; }, err));
    sc.join();
    CHECK(sc.state() == ScanState::Finished);
    CHECK_EQ(cb.load(), 1);
    CHECK_EQ(sc.snapshot(0).total_bytes, fx.expected[""]);
    // Restart on the same Scanner, stop immediately.
    CHECK(sc.start(root, nullptr, err));
    sc.request_stop();
    sc.join();
    CHECK(sc.state() == ScanState::Stopped || sc.state() == ScanState::Finished);
    CHECK(!sc.start((base / "nope").u8string(), nullptr, err));
  }

  // Remove the links themselves first: remove_all would otherwise walk through a
  // junction (Windows) into its target and never finish.
  std::error_code ec;
  fs::remove(fx.root / "link_to_other", ec);
  fs::remove(fx.root / "alpha" / "loop_to_root", ec);
  fs::remove_all(base, ec);
  return finish("test_walker");
}
