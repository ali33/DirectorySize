// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Edge cases for the walkers: unreadable directories, hard links, very long
// paths, stopping a multi-threaded scan half-way, d_type-less filesystems, -x.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define GETPID _getpid
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define GETPID getpid
#endif

#include "check.h"
#include "core/scanner.h"
#include "platform/walker.h"

namespace fs = std::filesystem;
using namespace dirsize;

static fs::path P(const std::string& u8) { return fs::u8path(u8); }

static void write_file(const fs::path& p, size_t n) {
  std::ofstream f(p, std::ios::binary);
  std::string data(n, 'x');
  f.write(data.data(), static_cast<std::streamsize>(n));
}

struct Run {
  bool root_ok = false, complete = false;
  SizeSnapshot snap;
};

static Run scan(const std::string& root, const WalkOptions& o, bool stop_first = false) {
  Run r;
  auto w = make_default_walker(o);
  std::string err;
  auto tree = w->make_tree(root, err);
  if (!tree) return r;
  std::atomic<bool> stop{stop_first};
  WalkOutcome out = w->walk(root, *tree, stop);
  r.root_ok = out.root_ok;
  r.complete = out.complete;
  r.snap = tree->snapshot(1000000);
  return r;
}

static std::vector<WalkOptions> option_sets() {
  std::vector<WalkOptions> v;
  WalkOptions a;
  a.threads = 4;
#ifdef _WIN32
  a.api = ListApi::Find;
  v.push_back(a);
  a.api = ListApi::Nt;
  v.push_back(a);
#else
  v.push_back(a);
  a.threads = 1;
  v.push_back(a);
#endif
  return v;
}

// ---- 1) a directory that cannot be listed is counted and reported ------------
static void test_inaccessible(const fs::path& base) {
  fs::path root = base / "inacc";
  fs::path locked = root / "locked_dir";
  fs::create_directories(locked);
  write_file(root / "ok.bin", 1000);
  write_file(locked / "secret.bin", 77);
  bool denied = false;
#ifdef _WIN32
  // Deny "list folder" to Everyone (S-1-1-0). The owner can still reset the ACL afterwards.
  std::wstring cmd = L"icacls \"" + locked.wstring() + L"\" /deny *S-1-1-0:(OI)(CI)(RD) >nul 2>nul";
  denied = _wsystem(cmd.c_str()) == 0;
  CHECK(denied);
#else
  if (geteuid() == 0) {
    std::printf("note: running as root, chmod 000 does not block it: inaccessible test skipped\n");
  } else {
    fs::permissions(locked, fs::perms::none);
    denied = true;
  }
#endif
  if (denied) {
    // Some environments (elevated CI runners, CAP_DAC_OVERRIDE) list the directory anyway:
    // then the permission cannot be enforced here and the check would test the environment.
    std::error_code pec;
    fs::directory_iterator probe(locked, pec);
    if (!pec) {
      std::printf("note: this environment still lists the denied directory: inaccessible test skipped\n");
      denied = false;
    }
  }
  if (denied) {
    for (const WalkOptions& o : option_sets()) {
      Run r = scan(root.u8string(), o);
      CHECK(r.root_ok);
      CHECK(r.complete);
      CHECK_EQ(r.snap.inaccessible, uint64_t{1});
      CHECK_EQ(r.snap.total_bytes, uint64_t{1000});  // the locked file is not counted
      CHECK_EQ(r.snap.error_samples.size(), size_t{1});
      if (!r.snap.error_samples.empty()) CHECK(r.snap.error_samples[0].find("locked_dir") != std::string::npos);
    }
  }
#ifdef _WIN32
  std::wstring undo = L"icacls \"" + locked.wstring() + L"\" /remove:d *S-1-1-0 >nul 2>nul";
  _wsystem(undo.c_str());
#else
  fs::permissions(locked, fs::perms::owner_all);
#endif
}

// ---- 2) hard links counted once (opt-in on Windows, default on POSIX) -----------
static void test_hardlinks(const fs::path& base) {
  fs::path root = base / "hl";
  fs::create_directories(root / "a");
  fs::create_directories(root / "b");
  write_file(root / "a" / "data.bin", 4000);
  std::error_code ec;
  fs::create_hard_link(root / "a" / "data.bin", root / "b" / "link.bin", ec);
  write_file(root / "b" / "own.bin", 10);
  if (ec) {
    std::printf("note: hard links unavailable here (%s): test skipped\n", ec.message().c_str());
    return;
  }
  WalkOptions once;
  once.threads = 4;
  once.dedup_hardlinks = 1;
  Run r1 = scan(root.u8string(), once);
  CHECK_EQ(r1.snap.total_bytes, uint64_t{4010});  // shared file once
  CHECK_EQ(r1.snap.files, uint64_t{2});
  WalkOptions all = once;
  all.dedup_hardlinks = 0;
  Run r2 = scan(root.u8string(), all);
  CHECK_EQ(r2.snap.total_bytes, uint64_t{8010});  // every link counted
  CHECK_EQ(r2.snap.files, uint64_t{3});
}

// ---- 3) paths longer than 260 chars (Windows) / PATH_MAX (POSIX) -------------
#ifdef _WIN32
static void test_long_path(const fs::path& base) {
  fs::path root = base / "longp";
  fs::path deep = root;
  for (char c : {'a', 'b', 'c', 'd', 'e'}) deep /= std::string(90, c);
  // \\?\ lets the test itself create what the classic API cannot.
  std::wstring prefixed = L"\\\\?\\" + deep.wstring();
  std::error_code ec;
  fs::create_directories(fs::path(prefixed), ec);
  CHECK(!ec);
  write_file(fs::path(prefixed) / "f.bin", 123);
  for (const WalkOptions& o : option_sets()) {
    Run r = scan(root.u8string(), o);
    CHECK(r.root_ok);
    CHECK_EQ(r.snap.total_bytes, uint64_t{123});
    CHECK_EQ(r.snap.dirs, uint64_t{6});
    CHECK_EQ(r.snap.inaccessible, uint64_t{0});
    size_t longest = 0;
    for (auto& e : r.snap.top) longest = std::max(longest, e.path.size());
    CHECK(longest > 400);
  }
  // std::filesystem::remove_all does not terminate on these paths with this toolchain: let cmd do it.
  std::wstring rm = L"cmd /c rmdir /s /q \"\\\\?\\" + root.wstring() + L"\" >nul 2>nul";
  _wsystem(rm.c_str());
}
#else
static void test_long_path(const fs::path& base) {
  fs::path root = base / "longp";
  fs::create_directories(root);
  int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY);
  CHECK(fd >= 0);
  std::vector<int> fds{fd};
  std::vector<std::string> names;
  bool built = true;
  for (int i = 0; i < 60 && built; ++i) {  // 60 x 100 chars = 6000 > PATH_MAX (4096)
    std::string nm(100, static_cast<char>('a' + i % 26));
    if (mkdirat(fds.back(), nm.c_str(), 0755) != 0) { built = false; break; }
    int nfd = openat(fds.back(), nm.c_str(), O_RDONLY | O_DIRECTORY);
    if (nfd < 0) { built = false; break; }
    names.push_back(nm);
    fds.push_back(nfd);
  }
  CHECK(built);
  int ffd = openat(fds.back(), "f.bin", O_CREAT | O_WRONLY, 0644);
  if (ffd >= 0) {
    std::string d(123, 'x');
    ssize_t wr = write(ffd, d.data(), d.size());
    CHECK(wr == 123);
    close(ffd);
  }
  for (const WalkOptions& o : option_sets()) {
    Run r = scan(root.string(), o);
    CHECK(r.root_ok);
    CHECK_EQ(r.snap.total_bytes, uint64_t{123});
    CHECK_EQ(r.snap.dirs, uint64_t{61});
    CHECK_EQ(r.snap.inaccessible, uint64_t{0});
    size_t longest = 0;
    for (auto& e : r.snap.top) longest = std::max(longest, e.path.size());
    CHECK(longest > 4096);
  }
  // Clean up with *at calls (no path-based call can reach this depth).
  if (ffd >= 0) unlinkat(fds.back(), "f.bin", 0);
  for (size_t i = names.size(); i-- > 0;) {
    unlinkat(fds[i], names[i].c_str(), AT_REMOVEDIR);
    close(fds[i + 1]);
  }
  close(fds[0]);
}
#endif

// ---- 4) stopping a multi-threaded scan half-way ----------------------------------
static void test_stop_midway(const fs::path& base) {
  fs::path root = base / "wide";
  for (int i = 0; i < 40; ++i)
    for (int j = 0; j < 30; ++j) {
      fs::path d = root / ("d" + std::to_string(i)) / ("s" + std::to_string(j));
      fs::create_directories(d);
      write_file(d / "f.bin", 10);
    }
  const uint64_t expect = 40 * 30 * 10;
  for (const WalkOptions& o : option_sets()) {
    // Already stopped: returns at once, result flagged partial.
    Run pre = scan(root.u8string(), o, /*stop_first=*/true);
    CHECK(pre.root_ok);
    CHECK(!pre.complete);
    // Uninterrupted: complete and exact.
    Run full = scan(root.u8string(), o);
    CHECK(full.complete);
    CHECK_EQ(full.snap.total_bytes, expect);
    // Stop raised from another thread after a short delay, many times over.
    for (int rep = 0; rep < 25; ++rep) {
      auto w = make_default_walker(o);
      std::string err;
      auto tree = w->make_tree(root.u8string(), err);
      std::atomic<bool> stop{false};
      std::thread flipper([&] {
        std::this_thread::sleep_for(std::chrono::microseconds(200 + 150 * rep));
        stop.store(true);
      });
      auto t0 = std::chrono::steady_clock::now();
      WalkOutcome out = w->walk(root.u8string(), *tree, stop);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      flipper.join();
      CHECK(out.root_ok);
      CHECK(ms < 5000);
      SizeSnapshot s = tree->snapshot(10);
      CHECK(s.total_bytes <= expect);
      if (out.complete) CHECK_EQ(s.total_bytes, expect);  // "complete" must really mean complete
    }
  }
  // Scanner state follows the same rule.
  Scanner sc(make_default_walker(WalkOptions{}));
  std::string err;
  CHECK(sc.start(root.u8string(), nullptr, err));
  sc.join();
  CHECK(sc.state() == ScanState::Finished);
  CHECK_EQ(sc.snapshot(0).total_bytes, expect);
}

#ifndef _WIN32
// ---- 5) filesystems without d_type, and -x -----------------------------------------
static void test_posix_modes(const fs::path& base) {
  fs::path root = base / "modes";
  fs::create_directories(root / "x" / "y");
  write_file(root / "x" / "y" / "f.bin", 500);
  write_file(root / "g.bin", 5);
  WalkOptions a;
  a.threads = 4;
  Run normal = scan(root.string(), a);
  WalkOptions b = a;
  b.ignore_d_type = true;  // behave as if every entry were DT_UNKNOWN
  Run unk = scan(root.string(), b);
  CHECK_EQ(normal.snap.total_bytes, uint64_t{505});
  CHECK_EQ(unk.snap.total_bytes, normal.snap.total_bytes);
  CHECK_EQ(unk.snap.dirs, normal.snap.dirs);
  CHECK_EQ(unk.snap.files, normal.snap.files);

  // -x on "/": /proc is a separate filesystem and must not be entered.
  struct stat rs, ps;
  if (stat("/", &rs) == 0 && stat("/proc/self", &ps) == 0 && rs.st_dev != ps.st_dev) {
    WalkOptions x;
    x.threads = 8;
    x.one_file_system = true;
    Run r = scan("/", x);
    CHECK(r.root_ok);
    bool entered = false;
    for (auto& e : r.snap.top)
      if (e.path.rfind("/proc", 0) == 0) entered = true;
    CHECK(!entered);
    CHECK(r.snap.skipped_links >= 1);
  } else {
    std::printf("note: /proc is not a separate filesystem here: -x check skipped\n");
  }
}
#endif

int main() {
  fs::path base = fs::temp_directory_path() / P("dirsize_extra_" + std::to_string(GETPID()) + "_Việt");
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base);

  std::fputs("test_inaccessible", stderr); std::fputc(10, stderr);
  test_inaccessible(base);
  std::fputs("test_hardlinks", stderr); std::fputc(10, stderr);
  test_hardlinks(base);
  std::fputs("test_long_path", stderr); std::fputc(10, stderr);
  test_long_path(base);
  std::fputs("test_stop_midway", stderr); std::fputc(10, stderr);
  test_stop_midway(base);
#ifndef _WIN32
  test_posix_modes(base);
#endif

  fs::remove_all(base, ec);
  return finish("test_walker_extra");
}
