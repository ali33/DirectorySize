// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Windows directory walker.
//   * Unicode paths, \\?\ long-path prefix.
//   * Two listing back-ends: FindFirstFileExW (FindExInfoBasic + LARGE_FETCH)
//     and NtQueryDirectoryFile (64 KB buffer, FileDirectoryInformation).
//   * Directory symlinks, junctions and mount points are NOT
//     followed, only counted. Other reparse points (e.g. OneDrive placeholders)
//     are counted with their reported size.
//   * Directories that cannot be listed are counted and sampled in the tree.
#include <windows.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "core/work_pool.h"
#include "platform/walker.h"

namespace dirsize {
namespace {

std::string to_utf8(const wchar_t* w, size_t n) {
  if (n == 0) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), &s[0], len, nullptr, nullptr);
  return s;
}
std::string to_utf8(const std::wstring& w) { return to_utf8(w.data(), w.size()); }

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
  return w;
}

std::string win_error_text(DWORD code) {
  wchar_t buf[512];
  DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
  while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'.')) --n;
  std::string s = n ? to_utf8(buf, n) : std::string("error");
  return s + " (" + std::to_string(code) + ")";
}

// Resolve user input to {display path, \\?\ path}. Returns false + error.
bool normalize(const std::string& root_utf8, std::wstring& display, std::wstring& lng, std::string& err) {
  std::wstring in = to_wide(root_utf8);
  while (!in.empty() && (in.front() == L' ' || in.front() == L'"')) in.erase(in.begin());
  while (!in.empty() && (in.back() == L' ' || in.back() == L'"')) in.pop_back();
  if (in.empty()) {
    err = "Chưa nhập đường dẫn";
    return false;
  }
  DWORD need = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
  if (need == 0) {
    err = win_error_text(GetLastError());
    return false;
  }
  std::wstring full(need, L'\0');
  DWORD got = GetFullPathNameW(in.c_str(), need, &full[0], nullptr);
  full.resize(got);

  std::wstring plain;  // without \\?\ prefix
  if (full.rfind(L"\\\\?\\UNC\\", 0) == 0) plain = L"\\\\" + full.substr(8);
  else if (full.rfind(L"\\\\?\\", 0) == 0) plain = full.substr(4);
  else plain = full;
  // Trim trailing backslashes, but keep a drive root like "C:\".
  while (plain.size() > 3 && plain.back() == L'\\') plain.pop_back();
  if (plain.size() == 2 && plain[1] == L':') plain += L'\\';

  lng = plain.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + plain.substr(2) : L"\\\\?\\" + plain;
  DWORD attr = GetFileAttributesW(lng.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    err = to_utf8(plain) + ": " + win_error_text(GetLastError());
    return false;
  }
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    err = to_utf8(plain) + ": không phải thư mục";
    return false;
  }
  display = plain;
  return true;
}

std::string display_of(const std::wstring& lng) {
  if (lng.rfind(L"\\\\?\\UNC\\", 0) == 0) return to_utf8(L"\\\\" + lng.substr(8));
  if (lng.rfind(L"\\\\?\\", 0) == 0) return to_utf8(lng.substr(4));
  return to_utf8(lng);
}

// ---- NtQueryDirectoryFile plumbing -----------------------------------------
struct IoStatusBlock {
  union {
    LONG Status;
    PVOID Pointer;
  };
  ULONG_PTR Information;
};
struct FileDirInfo {  // FILE_FULL_DIR_INFORMATION (EaSize carries the reparse tag)
  ULONG NextEntryOffset;
  ULONG FileIndex;
  LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
  LARGE_INTEGER EndOfFile;
  LARGE_INTEGER AllocationSize;
  ULONG FileAttributes;
  ULONG FileNameLength;
  ULONG EaSize;
  WCHAR FileName[1];
};
using NtQueryDirFn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, IoStatusBlock*, PVOID, ULONG, ULONG, BOOLEAN, PVOID, BOOLEAN);
using RtlNtToDosFn = ULONG(NTAPI*)(LONG);
constexpr LONG kStatusNoMoreFiles = static_cast<LONG>(0x80000006);
constexpr ULONG kFileDirectoryInformation = 2;  // FileFullDirectoryInformation

// Only symlinks and junctions/mount points can loop or leave the tree. Other reparse
// tags (OneDrive/cloud placeholders, dedup, ...) are scanned like ordinary directories.
constexpr DWORD kTagSymlink = 0xA000000C, kTagMountPoint = 0xA0000003;
inline bool is_link_tag(DWORD tag) { return tag == kTagSymlink || tag == kTagMountPoint; }

struct NtApi {
  NtQueryDirFn query = nullptr;
  RtlNtToDosFn to_dos = nullptr;
  NtApi() {
    HMODULE m = GetModuleHandleW(L"ntdll.dll");
    if (m) {
      query = reinterpret_cast<NtQueryDirFn>(reinterpret_cast<void*>(GetProcAddress(m, "NtQueryDirectoryFile")));
      to_dos = reinterpret_cast<RtlNtToDosFn>(reinterpret_cast<void*>(GetProcAddress(m, "RtlNtStatusToDosError")));
    }
  }
};

struct Work {
  SizeTree::NodeId id = 0;
  std::wstring path;  // \\?\ form
};

class WinWalker : public IDirWalker {
 public:
  explicit WinWalker(const WalkOptions& o) : opt_(o) {
    if (opt_.threads == 0) {
      unsigned hc = std::thread::hardware_concurrency();
      opt_.threads = std::min(16u, std::max(2u, hc));
    }
    if (opt_.api == ListApi::Default) opt_.api = nt_.query ? ListApi::Nt : ListApi::Find;
    if (opt_.api == ListApi::Nt && !nt_.query) opt_.api = ListApi::Find;
  }

  std::unique_ptr<SizeTree> make_tree(const std::string& root_utf8, std::string& error) override {
    std::wstring display, lng;
    if (!normalize(root_utf8, display, lng, error)) return nullptr;
    return std::unique_ptr<SizeTree>(new SizeTree(to_utf8(display), '\\'));
  }

  WalkOutcome walk(const std::string& root_utf8, SizeTree& tree, const std::atomic<bool>& stop) override {
    WalkOutcome out;
    std::wstring display, lng;
    if (!normalize(root_utf8, display, lng, out.root_error)) return out;
    out.root_ok = true;
    std::vector<Work> init;
    init.push_back(Work{SizeTree::kRoot, lng});
    const bool nt = opt_.api == ListApi::Nt;
    WorkPool<Work>::run(opt_.threads, std::move(init), stop, [&](Work& w, std::vector<Work>& children) {
      if (nt) scan_nt(w, children, tree, stop);
      else scan_find(w, children, tree, stop);
    });
    return out;
  }

 private:
  static void join_path(const std::wstring& base, const WCHAR* name, size_t n, std::wstring& out) {
    out = base;
    if (out.empty() || out.back() != L'\\') out += L'\\';
    out.append(name, n);
  }

  void scan_find(Work& w, std::vector<Work>& children, SizeTree& tree, const std::atomic<bool>& stop) {
    std::wstring pat = w.path;
    if (pat.back() != L'\\') pat += L'\\';
    pat += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
      tree.note_inaccessible(display_of(w.path), win_error_text(GetLastError()));
      return;
    }
    uint64_t bytes = 0, files = 0;
    DWORD err = 0;
    do {
      const WCHAR* nm = fd.cFileName;
      if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(fd.dwReserved0)) {
          tree.note_skipped_link();
          continue;
        }
        size_t n = wcslen(nm);
        Work c;
        c.id = tree.add_dir(w.id, to_utf8(nm, n));
        join_path(w.path, nm, n, c.path);
        children.push_back(std::move(c));
      } else {
        bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        ++files;
      }
      if (stop.load(std::memory_order_relaxed)) break;
    } while (FindNextFileW(h, &fd));
    if (!stop.load(std::memory_order_relaxed)) {
      err = GetLastError();
      if (err != ERROR_NO_MORE_FILES) tree.note_inaccessible(display_of(w.path), win_error_text(err));
    }
    FindClose(h);
    tree.add_files(w.id, bytes, files);
  }

  void scan_nt(Work& w, std::vector<Work>& children, SizeTree& tree, const std::atomic<bool>& stop) {
    alignas(16) char buf[64 * 1024];  // worker stack; thread_local was avoided after a heap-corruption crash in a static MinGW build (root cause not isolated)
    HANDLE h = CreateFileW(w.path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      tree.note_inaccessible(display_of(w.path), win_error_text(GetLastError()));
      return;
    }
    uint64_t bytes = 0, files = 0;
    bool restart = true;
    for (;;) {
      if (stop.load(std::memory_order_relaxed)) break;
      IoStatusBlock iosb{};
      LONG st = nt_.query(h, nullptr, nullptr, nullptr, &iosb, buf, static_cast<ULONG>(sizeof buf),
                          kFileDirectoryInformation, FALSE, nullptr, restart ? TRUE : FALSE);
      restart = false;
      if (st == kStatusNoMoreFiles) break;
      if (st < 0) {
        DWORD e = nt_.to_dos ? nt_.to_dos(st) : static_cast<DWORD>(st);
        tree.note_inaccessible(display_of(w.path), win_error_text(e));
        break;
      }
      const char* p = buf;
      for (;;) {
        const FileDirInfo* fi = reinterpret_cast<const FileDirInfo*>(p);
        size_t n = fi->FileNameLength / sizeof(WCHAR);
        const WCHAR* nm = fi->FileName;
        bool dot = (n == 1 && nm[0] == L'.') || (n == 2 && nm[0] == L'.' && nm[1] == L'.');
        if (!dot) {
          if (fi->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if ((fi->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(fi->EaSize)) {
              tree.note_skipped_link();
            } else {
              Work c;
              c.id = tree.add_dir(w.id, to_utf8(nm, n));
              join_path(w.path, nm, n, c.path);
              children.push_back(std::move(c));
            }
          } else {
            bytes += static_cast<uint64_t>(fi->EndOfFile.QuadPart);
            ++files;
          }
        }
        if (fi->NextEntryOffset == 0) break;
        p += fi->NextEntryOffset;
      }
    }
    CloseHandle(h);
    tree.add_files(w.id, bytes, files);
  }

  WalkOptions opt_;
  NtApi nt_;
};

}  // namespace

std::unique_ptr<IDirWalker> make_default_walker(const WalkOptions& opt) {
  return std::unique_ptr<IDirWalker>(new WinWalker(opt));
}

}  // namespace dirsize
