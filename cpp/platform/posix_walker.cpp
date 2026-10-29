// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// POSIX (Linux) directory walker.
//   * Directories are opened relative to their parent's file descriptor
//     (openat + O_NOFOLLOW + O_DIRECTORY): no absolute path is ever built for
//     system calls, so PATH_MAX does not apply and a directory swapped for a
//     symlink while scanning is not followed (no TOCTOU).
//   * File sizes via fstatat(dirfd, name, AT_SYMLINK_NOFOLLOW); d_type is used to
//     avoid stat on directories (stat only when DT_UNKNOWN or ignore_d_type).
//   * Symlinks are never followed; their own (lstat) size is counted like
//     `du --apparent-size` / `find -type l -printf %s` do, and they are counted
//     in skipped_links.
//   * Sizes are LOGICAL (st_size), not allocated (st_blocks).
//   * Hard links: a regular file with st_nlink > 1 is counted once, keyed by
//     (st_dev, st_ino), in whichever directory is reached first (like du).
//   * one_file_system: do not descend into directories on another st_dev (du -x).
//   * Unreadable directories (EACCES, ...) are counted and sampled.
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/work_pool.h"
#include "platform/walker.h"

namespace dirsize {
namespace {

// strerror_r exists in two incompatible flavours (GNU returns char*, XSI int).
inline const char* sr_result(int rc, const char* buf) { return rc == 0 ? buf : "unknown error"; }
inline const char* sr_result(const char* msg, const char*) { return msg ? msg : "unknown error"; }

std::string errno_text(int e) {
  char buf[200] = {0};
  const char* msg = sr_result(strerror_r(e, buf, sizeof buf), buf);
  return std::string(msg) + " (" + std::to_string(e) + ")";
}

std::string clean_root(std::string r) {
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  return r;
}

// An open directory kept alive until every child that needs it has been opened.
struct DirFd {
  int fd;
  explicit DirFd(int f) : fd(f) {}
  ~DirFd() {
    if (fd >= 0) close(fd);
  }
  DirFd(const DirFd&) = delete;
  DirFd& operator=(const DirFd&) = delete;
};

struct Work {
  SizeTree::NodeId id = 0;
  std::shared_ptr<DirFd> parent;  // null for the root
  std::string name;               // single component (root: the root path)
};

struct InoKey {
  dev_t dev;
  ino_t ino;
  bool operator==(const InoKey& o) const { return dev == o.dev && ino == o.ino; }
};
struct InoHash {
  size_t operator()(const InoKey& k) const {
    return static_cast<size_t>(k.ino * 0x9E3779B97F4A7C15ull) ^ static_cast<size_t>(k.dev);
  }
};
class InoSet {
 public:
  bool insert(dev_t d, ino_t i) {  // true if newly inserted
    InoKey k{d, i};
    Shard& s = shards_[InoHash()(k) % kShards];
    std::lock_guard<std::mutex> lk(s.mu);
    return s.set.insert(k).second;
  }
  void clear() {
    for (auto& s : shards_) {
      std::lock_guard<std::mutex> lk(s.mu);
      s.set.clear();
    }
  }

 private:
  static constexpr size_t kShards = 64;
  struct Shard {
    std::mutex mu;
    std::unordered_set<InoKey, InoHash> set;
  };
  Shard shards_[kShards];
};

class PosixWalker : public IDirWalker {
 public:
  explicit PosixWalker(const WalkOptions& o) : opt_(o) {
    if (opt_.threads == 0) {
      unsigned hc = std::thread::hardware_concurrency();
      opt_.threads = std::min(16u, std::max(2u, hc));
    }
    dedup_ = opt_.dedup_hardlinks != 0;  // default on: only files with st_nlink > 1 pay for it
    // Directory descriptors stay open while their children are queued: raise the soft limit.
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
      rl.rlim_cur = rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
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
    // The root is opened by path (following a symlink given by the user); everything below is openat-relative.
    int rfd = open(r.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rfd < 0) {
      out.root_error = r + ": " + errno_text(errno);
      return out;
    }
    std::shared_ptr<DirFd> root_dir = std::make_shared<DirFd>(rfd);
    struct stat st;
    if (fstat(rfd, &st) != 0) {
      out.root_error = r + ": " + errno_text(errno);
      return out;
    }
    out.root_ok = true;
    root_dev_ = st.st_dev;
    aborted_.store(false);
    inos_.clear();
    std::vector<Work> init;
    Work rw;
    rw.id = SizeTree::kRoot;
    rw.parent = nullptr;
    rw.name = ".";
    // Scan the root through its own fd: openat(rfd, ".") re-opens the same directory.
    rw.parent = root_dir;
    init.push_back(std::move(rw));
    root_dir.reset();  // the work item keeps it alive
    bool drained = WorkPool<Work>::run(opt_.threads, std::move(init), stop,
                                       [&](Work& w, std::vector<Work>& children) { scan(w, children, tree, stop); });
    out.complete = drained && !aborted_.load();
    return out;
  }

 private:
  void scan(Work& w, std::vector<Work>& children, SizeTree& tree, const std::atomic<bool>& stop) {
    int fd = openat(w.parent->fd, w.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      tree.note_inaccessible(tree.path_of(w.id), errno_text(errno));
      return;
    }
    auto self = std::make_shared<DirFd>(fd);  // handed to children; closed when the last one has opened itself
    int rfd = dup(fd);                        // fdopendir() takes ownership of its descriptor
    DIR* d = rfd >= 0 ? fdopendir(rfd) : nullptr;
    if (!d) {
      int e = errno;
      if (rfd >= 0) close(rfd);
      tree.note_inaccessible(tree.path_of(w.id), errno_text(e));
      return;
    }
    uint64_t bytes = 0, files = 0;
    bool cut = false;
    errno = 0;
    while (struct dirent* de = readdir(d)) {
      const char* nm = de->d_name;
      if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) {
        errno = 0;
        continue;
      }
      if (stop.load(std::memory_order_relaxed)) {
        cut = true;
        break;
      }
      unsigned char t = opt_.ignore_d_type ? static_cast<unsigned char>(DT_UNKNOWN) : de->d_type;
      struct stat st;
      bool have_stat = false;
      if (t == DT_UNKNOWN) {
        if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          tree.note_inaccessible(tree.path_of(w.id) + "/" + nm, errno_text(errno));
          errno = 0;
          continue;
        }
        have_stat = true;
        t = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_FIFO;
      }
      if (t == DT_DIR) {
        if (opt_.one_file_system) {
          if (!have_stat && fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            tree.note_inaccessible(tree.path_of(w.id) + "/" + nm, errno_text(errno));
            errno = 0;
            continue;
          }
          if (st.st_dev != root_dev_) {
            tree.note_skipped_link();
            errno = 0;
            continue;
          }
        }
        Work c;
        c.id = tree.add_dir(w.id, nm);
        c.parent = self;
        c.name = nm;
        children.push_back(std::move(c));
      } else if (t == DT_REG || t == DT_LNK) {
        if (!have_stat && fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
          errno = 0;  // vanished between readdir and stat
          continue;
        }
        if (t == DT_LNK) tree.note_skipped_link();
        if (dedup_ && S_ISREG(st.st_mode) && st.st_nlink > 1 && !inos_.insert(st.st_dev, st.st_ino)) {
          errno = 0;  // another link to a file already counted
          continue;
        }
        bytes += static_cast<uint64_t>(st.st_size);
        ++files;
      }
      errno = 0;
    }
    if (!cut && errno != 0) tree.note_inaccessible(tree.path_of(w.id), errno_text(errno));
    closedir(d);
    if (cut) aborted_.store(true);
    tree.add_files(w.id, bytes, files);
  }

  WalkOptions opt_;
  bool dedup_ = true;
  dev_t root_dev_ = 0;
  InoSet inos_;
  std::atomic<bool> aborted_{false};
};

}  // namespace

std::unique_ptr<IDirWalker> make_default_walker(const WalkOptions& opt) {
  return std::unique_ptr<IDirWalker>(new PosixWalker(opt));
}

}  // namespace dirsize
