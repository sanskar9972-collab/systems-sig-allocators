// spsc_queue.hpp - Phase 2: bounded lock-free Single-Producer Single-Consumer ring.
//
// Ownership / memory ordering
//   tail_ is written ONLY by the producer, head_ ONLY by the consumer.
//   * producer: write slot, then tail_.store(release)   -> publishes the slot
//   * consumer: tail_.load(acquire), then read slot      -> sees the slot's data
//   * consumer: read slot, then head_.store(release)     -> hands the slot back
//   * producer: head_.load(acquire), then overwrite slot -> never races the read
//   An index's owner reads its own index with relaxed (nobody else writes it).
//
// Indices are monotonically increasing size_t values (they wrap naturally in
// unsigned arithmetic), slot = index & mask, so all `capacity` slots are usable
// and full/empty are told apart by (tail - head).
//
// False sharing: head_ and tail_ (plus each side's private cached copy of the
// other index) live on separate 64-byte cache lines via alignas(64).
// The cached copies mean the producer only touches the consumer's cache line
// when the queue *looks* full (and vice-versa), cutting cache-line ping-pong.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common.hpp"

template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity)
      : cap_(round_up_pow2(capacity)), mask_(cap_ - 1), buf_(new T[cap_]) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer thread only.
  bool try_push(const T& v) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_cache_ == cap_) {                       // looks full: refresh
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == cap_) return false;         // really full
    }
    buf_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);       // publish
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {                              // looks empty: refresh
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return false;                // really empty
    }
    out = buf_[h & mask_];
    head_.store(h + 1, std::memory_order_release);       // free the slot
    return true;
  }

  // Consumer thread only. Pops up to `max` items with ONE release store.
  std::size_t try_pop_batch(T* out, std::size_t max) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    std::size_t avail = tail_cache_ - h;
    if (avail == 0) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      avail = tail_cache_ - h;
      if (avail == 0) return 0;
    }
    const std::size_t n = avail < max ? avail : max;
    for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(h + i) & mask_];
    head_.store(h + n, std::memory_order_release);
    return n;
  }

  std::size_t capacity() const noexcept { return cap_; }

  // For tests: are the two shared atomics really on different cache lines?
  bool indices_on_distinct_cache_lines() const noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(&head_) / kCacheLine;
    const auto b = reinterpret_cast<std::uintptr_t>(&tail_) / kCacheLine;
    return a != b;
  }

 private:
  // Read-only after construction -> shared freely by both threads.
  const std::size_t cap_;
  const std::size_t mask_;
  std::unique_ptr<T[]> buf_;

  // Consumer-owned cache line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;  // consumer's private copy of tail_

  // Producer-owned cache line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;  // producer's private copy of head_
};
