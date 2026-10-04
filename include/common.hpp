// common.hpp - small shared helpers (cache-line size, spin/backoff, pinning)
#pragma once
#include <cstddef>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// Typical x86-64 / ARMv8 cache line size. Used for alignas() to stop false sharing.
inline constexpr std::size_t kCacheLine = 64;

// CPU hint used inside spin loops (reduces power / helps the sibling hyper-thread).
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

// Spin for a while, then yield to the scheduler. Pure spinning is fastest when
// producer and consumer own a core each, but it starves the other thread on a
// machine with fewer cores than threads, so we fall back to yield() after a
// short spin.
class Backoff {
 public:
  void pause() noexcept {
    if (spins_ < kSpinLimit) {
      ++spins_;
      cpu_relax();
    } else {
      std::this_thread::yield();
    }
  }
  void reset() noexcept { spins_ = 0; }

 private:
  static constexpr unsigned kSpinLimit = 128;
  unsigned spins_ = 0;
};

// Pin the calling thread to one CPU core (Linux only). Returns true on success.
inline bool pin_this_thread_to_core(int core) noexcept {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)core;
  return false;
#endif
}

constexpr std::size_t round_up_pow2(std::size_t v) noexcept {
  std::size_t p = 2;
  while (p < v) p <<= 1;
  return p;
}
