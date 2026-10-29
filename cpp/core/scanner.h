// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// OS-independent: runs a walker on a background thread, exposes a stop flag
// and thread-safe snapshots for UIs to poll.
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/size_tree.h"
#include "platform/walker.h"

namespace dirsize {

enum class ScanState { Idle, Running, Finished, Stopped, Failed };

class Scanner {
 public:
  // Called on the scanner thread right before it ends. Keep it short (e.g.
  // PostMessage); do not call start()/join() from inside it.
  using FinishCallback = std::function<void(ScanState)>;

  explicit Scanner(std::unique_ptr<IDirWalker> walker);
  ~Scanner();  // requests stop and joins

  // Starts a new scan (any previous one is stopped and joined first).
  // Returns false and fills `error` if the root is not a usable directory.
  bool start(const std::string& root_utf8, FinishCallback on_finish, std::string& error);

  void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }  // signal-handler safe
  void join();

  ScanState state() const { return state_.load(); }
  bool running() const { return state_.load() == ScanState::Running; }
  std::string error() const;  // set when state()==Failed
  std::chrono::milliseconds elapsed() const;
  SizeSnapshot snapshot(size_t top_n) const;

 private:
  std::unique_ptr<IDirWalker> walker_;
  mutable std::mutex mu_;
  std::shared_ptr<SizeTree> tree_;
  std::string error_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<ScanState> state_{ScanState::Idle};
  std::chrono::steady_clock::time_point t0_{}, t1_{};
  std::atomic<bool> timing_done_{true};
};

}  // namespace dirsize
