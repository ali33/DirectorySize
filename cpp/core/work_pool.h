// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// OS-independent: tiny work pool for tree walkers.
//
// Work items are directories. The processing function may emit new items
// (sub-directories) into `out`. Items are taken LIFO so memory stays bounded
// (depth-first-ish). run() returns when no item is queued or in flight, or
// when `stop` is set; the first exception thrown by a worker is rethrown.
// If a helper thread cannot be created the pool simply runs with fewer threads.
#pragma once
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace dirsize {

template <class T>
class WorkPool {
 public:
  using Fn = std::function<void(T& item, std::vector<T>& out)>;

  // Returns true when every queued item was processed (false: abandoned because of `stop`).
  static bool run(unsigned threads, std::vector<T> initial, const std::atomic<bool>& stop, Fn fn) {
    if (threads == 0) threads = 1;
    State st{std::move(initial), stop, std::move(fn)};
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) {
      try {
        pool.emplace_back([&st] { worker(st); });
      } catch (const std::system_error&) {
        break;  // could not create more threads: carry on with what we have
      }
    }
    worker(st);  // calling thread participates
    for (auto& t : pool) t.join();
    if (st.error) std::rethrow_exception(st.error);
    return st.q.empty();
  }

 private:
  struct State {
    State(std::vector<T> q0, const std::atomic<bool>& s, Fn f)
        : q(std::move(q0)), stop(s), fn(std::move(f)) {}
    std::mutex mu;
    std::condition_variable cv;
    std::vector<T> q;
    unsigned active = 0;
    const std::atomic<bool>& stop;
    Fn fn;
    std::exception_ptr error;
  };

  static void record_error(State& st) {
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.error) st.error = std::current_exception();
  }

  static void worker(State& st) {
    std::vector<T> out;
    for (;;) {
      T item;
      try {
        std::unique_lock<std::mutex> lk(st.mu);
        st.cv.wait(lk, [&] {
          return !st.q.empty() || (st.active == 0) || st.stop.load(std::memory_order_relaxed) || st.error;
        });
        if (st.stop.load(std::memory_order_relaxed) || st.error || st.q.empty()) {
          // Nothing left to take (stop/error, or queue empty with nobody active).
          st.cv.notify_all();
          return;
        }
        item = std::move(st.q.back());
        st.q.pop_back();
        ++st.active;
      } catch (...) {
        record_error(st);
        st.cv.notify_all();
        return;
      }
      // From here `active` was incremented: it MUST be decremented on every path.
      out.clear();
      try {
        st.fn(item, out);
      } catch (...) {
        record_error(st);
      }
      {
        std::lock_guard<std::mutex> lk(st.mu);
        try {
          for (auto& o : out) st.q.push_back(std::move(o));
        } catch (...) {
          if (!st.error) st.error = std::current_exception();
        }
        --st.active;
        // New work -> wake sleepers; finished with nothing left -> wake all to exit.
        st.cv.notify_all();
      }
    }
  }
};

}  // namespace dirsize
