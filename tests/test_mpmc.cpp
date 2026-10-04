#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "mpmc_queue.hpp"
#include "test_util.hpp"

int main() {
  // --- single-threaded: FIFO / full / empty
  {
    MpmcQueue<int> q(4);
    int x;
    CHECK(!q.try_pop(x));
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(4));
    for (int i = 0; i < 4; ++i) { CHECK(q.try_pop(x)); CHECK(x == i); }
    CHECK(!q.try_pop(x));
  }

  // --- 4 producers x 4 consumers: every value delivered exactly once
  {
    constexpr int P = 4, C = 4;
    constexpr std::uint64_t PER = 500'000, N = P * PER;
    MpmcQueue<std::uint64_t> q(1024);
    std::atomic<std::uint64_t> taken{0}, sum{0};
    std::vector<std::atomic<std::uint8_t>> seen(N);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    std::vector<std::thread> ts;
    for (int p = 0; p < P; ++p)
      ts.emplace_back([&, p] {
        Backoff b;
        for (std::uint64_t i = 0; i < PER; ++i) {
          const std::uint64_t v = p * PER + i;
          b.reset();
          while (!q.try_push(v)) b.pause();
        }
      });
    for (int c = 0; c < C; ++c)
      ts.emplace_back([&] {
        Backoff b;
        std::uint64_t local = 0;
        while (taken.load(std::memory_order_relaxed) < N) {
          std::uint64_t v;
          if (q.try_pop(v)) {
            CHECK(seen[v].fetch_add(1, std::memory_order_relaxed) == 0);  // no duplicates
            local += v;
            taken.fetch_add(1, std::memory_order_relaxed);
            b.reset();
          } else {
            b.pause();
          }
        }
        sum.fetch_add(local, std::memory_order_relaxed);
      });
    for (auto& t : ts) t.join();
    CHECK(taken.load() == N);
    CHECK(sum.load() == N * (N - 1) / 2);  // nothing lost
    for (auto& s : seen) CHECK(s.load() == 1);
  }

  std::puts("test_mpmc: all checks passed");
  return 0;
}
