// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// POSIX (Linux) directory walker.
//   * openat-free path items + fdopendir/readdir; file sizes via
//     fstatat(dirfd, name, AT_SYMLINK_NOFOLLOW) relative to the open dir.
//   * d_type is used to avoid stat on directories (stat only when DT_UNKNOWN).
//   * Symlinks are never followed; their own (lstat) size is counted like
//     `du --apparent-size` / `find -type l -printf %s` do, and they are counted
//     in skipped_links.
//   * Sizes are LOGICAL (st_size), not allocated (st_blocks).
//   * one_file_system: do not descend into directories on another st_dev (du -x).
//   * Unreadable directories (EACCES, ...) are counted and sampled.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/work_pool.h"
#include "platform/walker.h"

namespace dirsize {
namespace {

struct Work {
  SizeTree::NodeId id = 0;
  std::string path;
};

std::string errno_text(int e) { return std::string(std::strerror(e)) + " (" + std::to_string(e) + ")"; }

std::string clean_root(std::string r) {
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  return r;
}

class PosixWalker : public IDirWalker {
 public:
  explicit PosixWalker(const WalkOptions& o) : opt_(o) {
    if (opt_.threads == 0) {
      unsigned hc = std::thread::hardware_concurrency();
      opt_.threads = std::min(16u, std::max(2u, hc));
    }
  }

  std::unique_ptr<SizeTree> make_tree(const std::string& root_utf8, std::string& error) override {
    std::string r = clean_root(root_utf8);
    struct stat st;
    if (r.empty()) {
      error = "Chưa nhập đường dẫn";
      return nullptr;
    }
    if (stat(r.c_str(), &st) != 0) {
      error = r + ": " + errno_text(errno);
      return nullptr;
    }
    if (!S_ISDIR(st.st_mode)) {
      error = r + ": không phải thư mục";
      return nullptr;
    }
    return std::unique_ptr<SizeTree>(new SizeTree(r, '/'));
  }

  WalkOutcome walk(const std::string& root_utf8, SizeTree& tree, const std::atomic<bool>& stop) override {
    WalkOutcome out;
    std::string r = clean_root(root_utf8);
    struct stat st;
    if (stat(r.c_str(), &st) != 0) {
      out.root_error = r + ": " + errno_text(errno);
      return out;
    }
    out.root_ok = true;
    const dev_t root_dev = st.st_dev;
    std::vector<Work> init;
    init.push_back(Work{SizeTree::kRoot, r});
    WorkPool<Work>::run(opt_.threads, std::move(init), stop, [&](Work& w, std::vector<Work>& children) {
      scan(w, children, tree, stop, root_dev);
    });
    return out;
  }

 private:
  void scan(Work& w, std::vector<Work>& children, SizeTree& tree, const std::atomic<bool>& stop, dev_t root_dev) {
    int fd = open(w.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      tree.note_inaccessible(w.path, errno_text(errno));
      return;
    }
    DIR* d = fdopendir(fd);
    if (!d) {
      int e = errno;
      close(fd);
      tree.note_inaccessible(w.path, errno_text(e));
      return;
    }
    uint64_t bytes = 0, files = 0;
    errno = 0;
    while (struct dirent* de = readdir(d)) {
      const char* nm = de->d_name;
      if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) {
        errno = 0;
        continue;
      }
      if (stop.load(std::memory_order_relaxed)) break;
      unsigned char t = de->d_type;
      struct stat st;
      bool have_stat = false;
      if (t == DT_UNKNOWN) {
        if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          tree.note_inaccessible(w.path + "/" + nm, errno_text(errno));
          errno = 0;
          continue;
        }
        have_stat = true;
        t = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_FIFO;
      }
      if (t == DT_DIR) {
        if (opt_.one_file_system) {
          if (!have_stat && fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            tree.note_inaccessible(w.path + "/" + nm, errno_text(errno));
            errno = 0;
            continue;
          }
          if (st.st_dev != root_dev) {
            tree.note_skipped_link();
            errno = 0;
            continue;
          }
        }
        Work c;
        c.id = tree.add_dir(w.id, nm);
        c.path = w.path;
        if (c.path.back() != '/') c.path += '/';
        c.path += nm;
        children.push_back(std::move(c));
      } else if (t == DT_REG || t == DT_LNK) {
        if (!have_stat && fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          errno = 0;  // vanished between readdir and stat
          continue;
        }
        bytes += static_cast<uint64_t>(st.st_size);
        ++files;
        if (t == DT_LNK) tree.note_skipped_link();
      }
      errno = 0;
    }
    if (errno != 0) tree.note_inaccessible(w.path, errno_text(errno));
    closedir(d);
    tree.add_files(w.id, bytes, files);
  }

  WalkOptions opt_;
};

}  // namespace

std::unique_ptr<IDirWalker> make_default_walker(const WalkOptions& opt) {
  return std::unique_ptr<IDirWalker>(new PosixWalker(opt));
}

}  // namespace dirsize
