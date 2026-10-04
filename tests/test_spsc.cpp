#include <cstdint>
#include <thread>
#include <vector>

#include "spsc_queue.hpp"
#include "test_util.hpp"

int main() {
  static_assert(alignof(SpscQueue<int>) >= kCacheLine);

  // --- single-threaded semantics: FIFO, full, empty, capacity rounding
  {
    SpscQueue<int> q(5);  // rounds up to 8
    CHECK(q.capacity() == 8);
    CHECK(q.indices_on_distinct_cache_lines());
    int x;
    CHECK(!q.try_pop(x));
    for (int i = 0; i < 8; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // full: all `capacity` slots are usable
    for (int i = 0; i < 8; ++i) { CHECK(q.try_pop(x)); CHECK(x == i); }
    CHECK(!q.try_pop(x));
  }

  // --- wrap-around over many laps, with a partially filled queue
  {
    SpscQueue<std::uint64_t> q(4);
    std::uint64_t next_in = 0, next_out = 0, v;
    for (int round = 0; round < 10000; ++round) {
      for (int k = 0; k < 3; ++k) CHECK(q.try_push(next_in++));
      for (int k = 0; k < 3; ++k) { CHECK(q.try_pop(v)); CHECK(v == next_out++); }
    }
  }

  // --- batch pop
  {
    SpscQueue<int> q(16);
    for (int i = 0; i < 10; ++i) CHECK(q.try_push(i));
    int out[8];
    CHECK(q.try_pop_batch(out, 8) == 8);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == i);
    CHECK(q.try_pop_batch(out, 8) == 2);
    CHECK(out[0] == 8 && out[1] == 9);
    CHECK(q.try_pop_batch(out, 8) == 0);
  }

  // --- two threads: 5M items must arrive complete, in order, exactly once
  {
    constexpr std::uint64_t N = 5'000'000;
    SpscQueue<std::uint64_t> q(1024);
    std::thread prod([&] {
      Backoff b;
      for (std::uint64_t i = 0; i < N; ++i) {
        b.reset();
        while (!q.try_push(i)) b.pause();
      }
    });
    std::uint64_t expect = 0;
    Backoff b;
    while (expect < N) {
      std::uint64_t v;
      if (q.try_pop(v)) { CHECK(v == expect); ++expect; b.reset(); }
      else b.pause();
    }
    prod.join();
    std::uint64_t v;
    CHECK(!q.try_pop(v));
  }

  // --- two threads, batch consumer
  {
    constexpr std::uint64_t N = 3'000'000;
    SpscQueue<std::uint64_t> q(256);
    std::thread prod([&] {
      Backoff b;
      for (std::uint64_t i = 0; i < N; ++i) { b.reset(); while (!q.try_push(i)) b.pause(); }
    });
    std::uint64_t expect = 0, buf[64];
    Backoff b;
    while (expect < N) {
      std::size_t n = q.try_pop_batch(buf, 64);
      if (!n) { b.pause(); continue; }
      b.reset();
      for (std::size_t i = 0; i < n; ++i) CHECK(buf[i] == expect++);
    }
    prod.join();
  }

  std::puts("test_spsc: all checks passed");
  return 0;
}
