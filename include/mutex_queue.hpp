// mutex_queue.hpp - baseline: bounded std::queue guarded by a std::mutex.
// Same try_push / try_pop_batch interface as the lock-free queues so the
// pipeline can be instantiated with any of them.
#pragma once
#include <cstddef>
#include <mutex>
#include <queue>

template <typename T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : cap_(capacity) {}

  bool try_push(const T& v) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.size() >= cap_) return false;
    q_.push(v);
    return true;
  }

  std::size_t try_pop_batch(T* out, std::size_t max) {
    std::lock_guard<std::mutex> lk(m_);  // one lock for the whole batch
    std::size_t n = 0;
    while (n < max && !q_.empty()) {
      out[n++] = q_.front();
      q_.pop();
    }
    return n;
  }

  std::size_t capacity() const noexcept { return cap_; }

 private:
  std::size_t cap_;
  std::mutex m_;
  std::queue<T> q_;
};
