// pipeline.hpp - Phase 3: integrated producer -> queue -> consumer pipeline.
//
// The pipeline is a template over two policies so the exact same producer and
// consumer code is measured with different building blocks:
//   AllocPolicy : ArenaPolicy (Phase 1)  | MallocPolicy (baseline)
//   Queue       : SpscQueue (Phase 2) | MpmcQueue (bonus) | MutexQueue (baseline)
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include "arena.hpp"
#include "common.hpp"

// Simulated network packet descriptor: exactly one 64-byte cache line.
struct alignas(kCacheLine) PacketMetadata {
  std::uint64_t seq;
  std::uint32_t src_ip, dst_ip;
  std::uint16_t src_port, dst_port, length;
  std::uint8_t proto, flags;
  std::uint64_t payload[5];
};
static_assert(sizeof(PacketMetadata) == 64, "one packet == one cache line");
static_assert(std::is_trivially_destructible_v<PacketMetadata>);

// Deterministic mock payload derived from the sequence number, so the expected
// checksum can be recomputed independently to verify the pipeline end to end.
inline void fill_packet(PacketMetadata* p, std::uint64_t seq) noexcept {
  const std::uint64_t x = seq * 0x9E3779B97F4A7C15ull + 0x1234567ull;
  p->seq = seq;
  p->src_ip = static_cast<std::uint32_t>(x);
  p->dst_ip = static_cast<std::uint32_t>(x >> 32);
  p->src_port = static_cast<std::uint16_t>(x >> 16);
  p->dst_port = static_cast<std::uint16_t>(x >> 40);
  p->length = static_cast<std::uint16_t>(64 + (x >> 52) % 1400);
  p->proto = (x & 1) ? 6 : 17;  // TCP / UDP
  p->flags = static_cast<std::uint8_t>(x >> 8);
  for (int i = 0; i < 5; ++i) p->payload[i] = x ^ (seq + static_cast<std::uint64_t>(i));
}

inline std::uint64_t packet_checksum(const PacketMetadata& p) noexcept {
  std::uint64_t s = p.seq;
  s = s * 31 + ((static_cast<std::uint64_t>(p.src_ip) << 32) | p.dst_ip);
  s = s * 31 + ((static_cast<std::uint64_t>(p.src_port) << 32) |
                (static_cast<std::uint64_t>(p.dst_port) << 16) | p.length);
  s = s * 31 + ((static_cast<std::uint64_t>(p.proto) << 8) | p.flags);
  for (int i = 0; i < 5; ++i) s = s * 31 + p.payload[i];
  return s;
}

// Reference checksum, computed single-threaded outside the timed region.
inline std::uint64_t expected_checksum(std::uint64_t total) noexcept {
  PacketMetadata tmp;
  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < total; ++i) {
    fill_packet(&tmp, i);
    sum += packet_checksum(tmp);
  }
  return sum;
}

// ---------------------------------------------------------------- policies --

// Phase 1 arena. Only the producer thread allocates / resets.
class ArenaPolicy {
 public:
  static constexpr const char* kName = "arena";
  ArenaPolicy(std::size_t burst, bool huge_pages)
      : arena_(burst * sizeof(PacketMetadata), huge_pages, /*prefault=*/true) {}

  PacketMetadata* acquire() noexcept {
    return static_cast<PacketMetadata*>(
        arena_.alloc(sizeof(PacketMetadata), alignof(PacketMetadata)));
  }
  void release(PacketMetadata*) noexcept {}  // individual frees don't exist

  // Called by the producer after it has pushed `produced` messages in total.
  // Wait until the consumer has finished every one of them, then recycle the
  // whole arena in O(1). The acquire load pairs with the consumer's release
  // store of `consumed`, so every consumer read of arena memory happens-before
  // the producer's next write to it.
  void end_of_burst(std::uint64_t produced, const std::atomic<std::uint64_t>& consumed) noexcept {
    Backoff b;
    while (consumed.load(std::memory_order_acquire) != produced) b.pause();
    arena_.reset();
  }
  const Arena& arena() const noexcept { return arena_; }

 private:
  Arena arena_;
};

// Baseline: one malloc/free per message (64-byte aligned like the arena's).
class MallocPolicy {
 public:
  static constexpr const char* kName = "malloc";
  MallocPolicy(std::size_t, bool) {}
  PacketMetadata* acquire() noexcept {
    return static_cast<PacketMetadata*>(std::aligned_alloc(kCacheLine, sizeof(PacketMetadata)));
  }
  void release(PacketMetadata* p) noexcept { std::free(p); }
  void end_of_burst(std::uint64_t, const std::atomic<std::uint64_t>&) noexcept {}
};

// ------------------------------------------------------------------ runner --

struct RunOptions {
  std::uint64_t total = 4'000'000;  // messages per run
  std::uint64_t burst = 1 << 16;    // messages between arena resets
  std::size_t queue_capacity = 1 << 16;
  std::size_t batch = 64;           // consumer batch size
  bool huge_pages = false;
  bool pin = false;                 // pin producer->core 0, consumer->core 1
};

struct RunResult {
  double seconds = 0;
  std::uint64_t checksum = 0;
  std::uint64_t consumed = 0;
};

template <class AllocPolicy, template <class> class Queue>
RunResult run_pipeline(const RunOptions& o) {
  AllocPolicy alloc(o.burst, o.huge_pages);
  Queue<PacketMetadata*> queue(o.queue_capacity);

  std::atomic<std::uint64_t> consumed{0};
  std::atomic<bool> done{false};
  std::atomic<bool> go{false};
  std::uint64_t checksum = 0;

  std::thread consumer([&] {
    if (o.pin) pin_this_thread_to_core(1);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    PacketMetadata* batch[256];
    const std::size_t maxb = o.batch < 256 ? o.batch : 256;
    std::uint64_t sum = 0, cnt = 0;
    Backoff idle;
    for (;;) {
      std::size_t n = queue.try_pop_batch(batch, maxb);
      if (n == 0) {
        if (!done.load(std::memory_order_acquire)) { idle.pause(); continue; }
        n = queue.try_pop_batch(batch, maxb);  // producer finished: final drain
        if (n == 0) break;
      }
      idle.reset();
      for (std::size_t i = 0; i < n; ++i) {
        sum += packet_checksum(*batch[i]);  // read the event, update running tally
        alloc.release(batch[i]);
      }
      cnt += n;
      consumed.store(cnt, std::memory_order_release);  // once per batch
    }
    checksum = sum;
  });

  if (o.pin) pin_this_thread_to_core(0);
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);

  Backoff full;
  for (std::uint64_t i = 0; i < o.total; ++i) {
    PacketMetadata* p = alloc.acquire();
    if (!p) std::abort();  // burst larger than arena - programming error
    fill_packet(p, i);
    full.reset();
    while (!queue.try_push(p)) full.pause();
    if ((i + 1) % o.burst == 0) alloc.end_of_burst(i + 1, consumed);
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  const auto t1 = std::chrono::steady_clock::now();

  RunResult r;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.checksum = checksum;
  r.consumed = consumed.load(std::memory_order_relaxed);
  return r;
}
