// mpmc_queue.hpp - Bonus: bounded lock-free Multi-Producer Multi-Consumer queue.
//
// Dmitry Vyukov's bounded MPMC design (1024cores). Every cell carries a sequence
// number that encodes whose turn it is:
//   seq == pos        -> cell is free, a producer that claims ticket `pos` may fill it
//   seq == pos + 1    -> cell is full, a consumer that claims ticket `pos` may drain it
//   seq == pos + cap  -> consumer finished; cell is free for the next lap
// Producers/consumers claim a ticket with compare_exchange_weak on the shared
// enqueue/dequeue counter; the CAS only decides *who* gets the ticket, the
// acquire/release on the cell's seq is what publishes the payload.
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

#include "common.hpp"

template <typename T>
class MpmcQueue {
 public:
  explicit MpmcQueue(std::size_t capacity)
      : cap_(round_up_pow2(capacity)), mask_(cap_ - 1), cells_(new Cell[cap_]) {
    for (std::size_t i = 0; i < cap_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;

  bool try_push(const T& v) noexcept {
    Cell* cell;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (dif == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
        // CAS failure reloaded `pos` for us; retry.
      } else if (dif < 0) {
        return false;  // cell still holds last lap's data -> queue full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);  // someone else got ahead
      }
    }
    cell->data = v;
    cell->seq.store(pos + 1, std::memory_order_release);  // publish to consumers
    return true;
  }

  bool try_pop(T& out) noexcept {
    Cell* cell;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const std::intptr_t dif =
          static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (dif == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (dif < 0) {
        return false;  // not yet filled -> queue empty
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
    out = cell->data;
    cell->seq.store(pos + mask_ + 1, std::memory_order_release);  // free for next lap
    return true;
  }

  std::size_t try_pop_batch(T* out, std::size_t max) noexcept {
    std::size_t n = 0;
    while (n < max && try_pop(out[n])) ++n;
    return n;
  }

  std::size_t capacity() const noexcept { return cap_; }

 private:
  struct Cell {
    std::atomic<std::size_t> seq;
    T data;
  };

  const std::size_t cap_;
  const std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
};
