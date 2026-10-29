// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
#include "core/size_tree.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dirsize {

static constexpr size_t kMaxErrorSamples = 20;

SizeTree::SizeTree(std::string root_display, char sep) : root_display_(std::move(root_display)), sep_(sep) {
  for (auto& c : chunks_) c.store(nullptr, std::memory_order_relaxed);
  ensure_chunk(0);
  Node& r = node(0);
  r.parent = 0;
  r.name = root_display_;
  r.ready.store(true, std::memory_order_release);
  count_.store(1, std::memory_order_release);
  published_.store(1, std::memory_order_release);
}

SizeTree::~SizeTree() {
  for (auto& c : chunks_) delete[] c.load(std::memory_order_relaxed);
}

void SizeTree::ensure_chunk(size_t c) {
  if (c >= kMaxChunks) throw std::length_error("too many directories");
  if (chunks_[c].load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(chunk_mu_);
  if (!chunks_[c].load(std::memory_order_relaxed)) chunks_[c].store(new Node[kChunkSize], std::memory_order_release);
}

SizeTree::NodeId SizeTree::add_dir(NodeId parent, const std::string& name) {
  NodeId id = count_.fetch_add(1, std::memory_order_acq_rel);
  ensure_chunk(id >> kChunkBits);
  Node& n = node(id);
  n.parent = parent;
  n.name = name;
  n.ready.store(true, std::memory_order_release);
  published_.fetch_add(1, std::memory_order_release);  // last: snapshot caches key off this
  return id;
}

void SizeTree::add_files(NodeId dir, uint64_t bytes, uint64_t file_count) {
  // Bytes first, counter last: a reader that sees the new counter sees the bytes too.
  if (bytes) node(dir).own.fetch_add(bytes, std::memory_order_relaxed);
  files_.fetch_add(file_count, std::memory_order_release);
}

void SizeTree::note_inaccessible(const std::string& display_path, const std::string& reason) {
  {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (error_samples_.size() < kMaxErrorSamples) error_samples_.push_back(display_path + ": " + reason);
  }
  inaccessible_.fetch_add(1, std::memory_order_release);
}

std::string SizeTree::path_of(NodeId id) const {
  std::vector<uint32_t> chain;
  for (uint32_t i = id; i != kRoot; i = node(i).parent) chain.push_back(i);
  std::string p = node(0).name;
  for (size_t q = chain.size(); q-- > 0;) {
    if (!p.empty() && p.back() != sep_) p += sep_;
    p += node(chain[q]).name;
  }
  return p;
}

SizeSnapshot SizeTree::snapshot(size_t top_n) const {
  std::lock_guard<std::mutex> snap_lock(snap_mu_);
  // Read the version BEFORE the data: every writer bumps its counter after
  // writing, so data read now is at least as new as `ver`.
  const uint64_t ver = version();
  if (ver == cache_version_ && top_n == cache_top_n_) return cache_;

  SizeSnapshot s;
  const uint32_t n = count_.load(std::memory_order_acquire);
  // Not-yet-published nodes (id handed out, fields still being written) are
  // skipped; ids are only ever referenced as parents after being published.
  std::vector<uint64_t>& total = buf_total_;
  std::vector<uint8_t>& ok = buf_ok_;
  total.assign(n, 0);
  ok.assign(n, 0);
  uint32_t live = 0;
  // Read flags from the highest id down: `ready` only ever flips false->true and a
  // child is created after its parent is published, so a ready child implies a
  // ready parent in this snapshot (totals never drop out of the roll-up).
  for (uint32_t i = n; i-- > 0;) {
    if (!chunks_[i >> kChunkBits].load(std::memory_order_acquire)) continue;  // chunk not allocated yet
    const Node& nd = node(i);
    if (nd.ready.load(std::memory_order_acquire)) {
      ok[i] = 1;
      ++live;
      total[i] = nd.own.load(std::memory_order_relaxed);
    }
  }
  // Children have larger ids than parents: one reverse pass rolls sizes up.
  for (uint32_t i = n; i-- > 1;)
    if (ok[i]) total[node(i).parent] += total[i];

  s.dirs = live;
  s.files = files_.load(std::memory_order_acquire);
  s.total_bytes = total[0];
  s.inaccessible = inaccessible_.load(std::memory_order_acquire);
  s.skipped_links = skipped_links_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(err_mu_);
    s.error_samples = error_samples_;
  }

  std::vector<uint32_t>& idx = buf_idx_;
  idx.clear();
  for (uint32_t i = 0; i < n; ++i)
    if (ok[i] && total[i] > 0) idx.push_back(i);
  auto cmp = [&](uint32_t a, uint32_t b) { return total[a] != total[b] ? total[a] > total[b] : a < b; };
  size_t k = std::min(top_n, idx.size());
  std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k), idx.end(), cmp);
  s.top.reserve(k);
  for (size_t j = 0; j < k; ++j) s.top.push_back(DirEntryInfo{path_of(idx[j]), total[idx[j]]});

  cache_ = s;
  cache_version_ = ver;
  cache_top_n_ = top_n;
  return s;
}

}  // namespace dirsize
