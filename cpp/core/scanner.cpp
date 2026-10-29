// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
#include "core/scanner.h"

#include <exception>
#include <utility>

namespace dirsize {

Scanner::Scanner(std::unique_ptr<IDirWalker> walker) : walker_(std::move(walker)) {}

Scanner::~Scanner() {
  request_stop();
  join();
}

void Scanner::join() {
  if (thread_.joinable()) thread_.join();
}

bool Scanner::start(const std::string& root_utf8, FinishCallback on_finish, std::string& error) {
  request_stop();
  join();
  std::unique_ptr<SizeTree> tree = walker_->make_tree(root_utf8, error);
  if (!tree) return false;
  std::shared_ptr<SizeTree> t(std::move(tree));
  {
    std::lock_guard<std::mutex> lk(mu_);
    tree_ = t;
    error_.clear();
  }
  stop_.store(false);
  t0_ = std::chrono::steady_clock::now();
  timing_done_.store(false);
  state_.store(ScanState::Running);
  thread_ = std::thread([this, root_utf8, t, cb = std::move(on_finish)]() {
    ScanState end = ScanState::Finished;
    try {
      WalkOutcome o = walker_->walk(root_utf8, *t, stop_);
      if (!o.root_ok) {
        std::lock_guard<std::mutex> lk(mu_);
        error_ = o.root_error;
        end = ScanState::Failed;
      } else if (!o.complete) {
        end = ScanState::Stopped;  // cut short by the stop flag; a stop that arrives after the last directory is Finished
      }
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lk(mu_);
      error_ = std::string("Internal error: ") + e.what();
      end = ScanState::Failed;
    } catch (...) {
      std::lock_guard<std::mutex> lk(mu_);
      error_ = "Internal error (unknown exception)";
      end = ScanState::Failed;
    }
    t1_ = std::chrono::steady_clock::now();
    timing_done_.store(true);
    state_.store(end);
    try {
      if (cb) cb(end);
    } catch (...) {
    }
  });
  return true;
}

std::string Scanner::error() const {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

std::chrono::milliseconds Scanner::elapsed() const {
  auto end = timing_done_.load() ? t1_ : std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - t0_);
}

SizeSnapshot Scanner::snapshot(size_t top_n) const {
  std::shared_ptr<SizeTree> t;
  {
    std::lock_guard<std::mutex> lk(mu_);
    t = tree_;
  }
  return t ? t->snapshot(top_n) : SizeSnapshot{};
}

}  // namespace dirsize
