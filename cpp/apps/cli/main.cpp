// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// dirsize-cli: console front-end (Windows and Linux).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#include "core/scanner.h"
#include "core/size_format.h"
#include "core/version.h"
#include "platform/walker.h"

using namespace dirsize;

namespace {

std::atomic<Scanner*> g_scanner{nullptr};
std::atomic<bool> g_interrupted{false};

void on_interrupt_common() {
  // Second interrupt: leave immediately (the worker may be stuck in slow I/O).
  if (g_interrupted.exchange(true)) std::_Exit(130);
  if (Scanner* s = g_scanner.load()) s->request_stop();  // only an atomic store: signal-safe
}

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    on_interrupt_common();
    return TRUE;
  }
  return FALSE;
}
#else
void sig_handler(int) { on_interrupt_common(); }
#endif

void out(const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}
void err(const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), stderr);
  std::fflush(stderr);
}

bool stderr_is_tty() {
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

std::vector<std::string> get_args(int argc, char** argv) {
  std::vector<std::string> v;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (!w) return v;
  for (int i = 1; i < n; ++i) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
    v.push_back(s);
  }
  LocalFree(w);
#else
  for (int i = 1; i < argc; ++i) v.emplace_back(argv[i]);
#endif
  return v;
}

void usage() {
  out(
      "dirsize-cli - tính dung lượng từng thư mục (đệ quy) và in các thư mục lớn nhất\n"
      "\n"
      "Cách dùng: dirsize-cli [đường_dẫn] [tuỳ chọn]\n"
      "  -t, --top N          số thư mục lớn nhất cần in (mặc định 20)\n"
      "  -j, --threads N      số luồng quét (mặc định: số nhân, tối đa 16)\n"
      "  -i, --interval MS    chu kỳ cập nhật tiến độ (mặc định 500)\n"
      "  -x, --one-file-system  không vượt sang mount khác (Linux)\n"
      "      --api find|nt    Windows: chọn API liệt kê (mặc định nt)\n"
      "      --hardlinks once|all  tệp có nhiều hard link: tính một lần (mặc định Linux) hay mọi lần (mặc định Windows)\n"
      "  -q, --quiet          không in tiến độ\n"
      "  -h, --help           trợ giúp\n"
      "      --version        phiên bản, bản quyền, giấy phép\n"
      "Ctrl+C dừng quét và in kết quả một phần. Mã thoát: 0 xong, 1 lỗi, 2 sai tham số, 130 bị dừng.\n");
}

// Number of code points (one console column each for the scripts we print), not bytes.
size_t display_width(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

std::string fmt_sec(std::chrono::milliseconds ms) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f s", static_cast<double>(ms.count()) / 1000.0);
  return b;
}

std::string progress_line(const SizeSnapshot& s, std::chrono::milliseconds el) {
  return "Đang quét... " + format_count(s.dirs) + " thư mục, " + format_count(s.files) + " tệp, " +
         format_size(s.total_bytes) + ", " + format_count(s.inaccessible) + " không truy cập được, " + fmt_sec(el);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  struct CodePageGuard {  // restore the user's console code pages on exit
    UINT out = GetConsoleOutputCP(), in = GetConsoleCP();
    ~CodePageGuard() {
      SetConsoleOutputCP(out);
      SetConsoleCP(in);
    }
  } cp_guard;
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  std::vector<std::string> args = get_args(argc, argv);
  std::string path = ".";
  bool have_path = false, quiet = false;
  size_t top = 20;
  int interval = 500;
  WalkOptions wo;

  auto need_num = [&](size_t& i, long& v) -> bool {
    if (i + 1 >= args.size()) return false;
    char* end = nullptr;
    v = std::strtol(args[i + 1].c_str(), &end, 10);
    if (!end || *end || v < 0) return false;
    ++i;
    return true;
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    long v = 0;
    if (a == "-h" || a == "--help" || a == "/?") {
      usage();
      return 0;
    } else if (a == "--version") {
      out(about_text());
      return 0;
    } else if (a == "-t" || a == "--top") {
      if (!need_num(i, v) || v == 0) { err("Sai giá trị cho --top\n"); return 2; }
      top = static_cast<size_t>(v);
    } else if (a == "-j" || a == "--threads") {
      if (!need_num(i, v) || v == 0 || v > 256) { err("Sai giá trị cho --threads\n"); return 2; }
      wo.threads = static_cast<unsigned>(v);
    } else if (a == "-i" || a == "--interval") {
      if (!need_num(i, v) || v < 50 || v > 3600000) { err("Sai giá trị cho --interval (50..3600000)\n"); return 2; }
      interval = static_cast<int>(v);
    } else if (a == "-x" || a == "--one-file-system") {
      wo.one_file_system = true;
    } else if (a == "-q" || a == "--quiet") {
      quiet = true;
    } else if (a == "--hardlinks") {
      if (i + 1 >= args.size() || (args[i + 1] != "once" && args[i + 1] != "all")) { err("--hardlinks nhận once hoặc all\n"); return 2; }
      wo.dedup_hardlinks = args[++i] == "once" ? 1 : 0;
    } else if (a == "--api") {
      if (i + 1 >= args.size() || (args[i + 1] != "find" && args[i + 1] != "nt")) { err("--api nhận find hoặc nt\n"); return 2; }
      wo.api = args[++i] == "find" ? ListApi::Find : ListApi::Nt;
    } else if (!a.empty() && a[0] == '-' && a.size() > 1) {
      err("Tuỳ chọn không hợp lệ: " + a + "\n");
      return 2;
    } else if (!have_path) {
      path = a;
      have_path = true;
    } else {
      err("Chỉ nhận một đường dẫn\n");
      return 2;
    }
  }

  Scanner scanner(make_default_walker(wo));
  g_scanner = &scanner;
  struct ScannerGuard {
    ~ScannerGuard() { g_scanner = nullptr; }
  } scanner_guard;
#ifdef _WIN32
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;  // do not turn slow syscalls into spurious EINTR errors
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  std::string e;
  if (!scanner.start(path, nullptr, e)) {
    err("Lỗi: " + e + "\n");
    return 1;
  }
  if (g_interrupted.load()) scanner.request_stop();  // signal arrived before start() cleared the flag
  const bool tty = stderr_is_tty();
  size_t last_len = 0;
  auto next_tick = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
  while (scanner.running()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // short naps: small scans end fast
    if (std::chrono::steady_clock::now() < next_tick) continue;
    next_tick += std::chrono::milliseconds(interval);
    if (!quiet && tty && scanner.running()) {
      std::string l = progress_line(scanner.snapshot(0), scanner.elapsed());
      size_t cols = display_width(l);  // columns, not UTF-8 bytes
      std::string pad = cols < last_len ? std::string(last_len - cols, ' ') : "";
      last_len = cols;
      err("\r" + l + pad);
    }
  }
  scanner.join();
  if (!quiet && tty && last_len) err("\r" + std::string(last_len, ' ') + "\r");

  ScanState st = scanner.state();
  if (st == ScanState::Failed) {
    err("Lỗi: " + scanner.error() + "\n");
    return 1;
  }
  SizeSnapshot s = scanner.snapshot(top);
  std::string o;
  for (const auto& d : s.top) {
    std::string sz = format_size(d.bytes);
    o += std::string(display_width(sz) < 11 ? 11 - display_width(sz) : 0, ' ') + sz + "  " + d.path + "\n";
  }
  o += "\n";
  o += (st == ScanState::Stopped ? "ĐÃ DỪNG (kết quả một phần): " : "Xong: ");
  o += format_size(s.total_bytes) + " (" + format_count(s.total_bytes) + " byte), " + format_count(s.dirs) +
       " thư mục, " + format_count(s.files) + " tệp, " + format_count(s.inaccessible) + " thư mục không truy cập được, " +
       format_count(s.skipped_links) + " liên kết/mount bỏ qua, " + fmt_sec(scanner.elapsed()) + "\n";
  out(o);
  if (!s.error_samples.empty()) {
    std::string m = "Thư mục không truy cập được (mẫu " + std::to_string(std::min<size_t>(s.error_samples.size(), 5)) + "/" +
                    format_count(s.inaccessible) + "):\n";
    for (size_t i = 0; i < s.error_samples.size() && i < 5; ++i) m += "  " + s.error_samples[i] + "\n";
    err(m);
  }
  return st == ScanState::Stopped ? 130 : 0;
}
