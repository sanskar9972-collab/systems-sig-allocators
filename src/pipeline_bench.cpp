// pipeline_bench.cpp - Phase 3 driver: runs the integrated pipeline and the
// baselines, verifies the checksum of every run and prints messages/second.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mpmc_queue.hpp"
#include "mutex_queue.hpp"
#include "pipeline.hpp"
#include "spsc_queue.hpp"

namespace {

struct Config {
  const char* name;
  RunResult (*fn)(const RunOptions&);
};

struct Stats {
  double best = 0, median = 0;
  bool ok = true;
};

Stats measure(const Config& c, const RunOptions& o, int runs, std::uint64_t expected) {
  c.fn(o);  // warm-up run (page cache, branch predictors, malloc arenas), discarded
  std::vector<double> rates;
  Stats s;
  for (int i = 0; i < runs; ++i) {
    RunResult r = c.fn(o);
    if (r.checksum != expected || r.consumed != o.total) s.ok = false;
    rates.push_back(static_cast<double>(o.total) / r.seconds);
  }
  std::sort(rates.begin(), rates.end());
  s.best = rates.back();
  s.median = rates[rates.size() / 2];
  return s;
}

void usage(const char* a0) {
  std::printf(
      "usage: %s [--messages N] [--burst B] [--queue Q] [--batch K] [--runs R]\n"
      "          [--huge-pages] [--pin]\n"
      "  --messages N   messages per run              (default 4000000)\n"
      "  --burst B      messages between arena resets (default 65536)\n"
      "  --queue Q      queue capacity, pow2-rounded  (default 65536)\n"
      "  --batch K      consumer batch size, <=256    (default 64)\n"
      "  --runs R       timed runs per config         (default 5)\n"
      "  --huge-pages   back the arena with 2 MiB pages (bonus)\n"
      "  --pin          pin producer/consumer to cores 0/1\n",
      a0);
}

}  // namespace

int main(int argc, char** argv) {
  RunOptions o;
  int runs = 5;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", flag); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--messages")) o.total = std::strtoull(need("--messages"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--burst")) o.burst = std::strtoull(need("--burst"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--queue")) o.queue_capacity = std::strtoull(need("--queue"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--batch")) o.batch = std::strtoull(need("--batch"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--runs")) runs = std::atoi(need("--runs"));
    else if (!std::strcmp(argv[i], "--huge-pages")) o.huge_pages = true;
    else if (!std::strcmp(argv[i], "--pin")) o.pin = true;
    else { usage(argv[0]); return std::strcmp(argv[i], "--help") ? 2 : 0; }
  }
  if (o.burst == 0 || o.total == 0 || runs < 1) { usage(argv[0]); return 2; }

  {  // report which page mode the arena actually got
    Arena probe(o.burst * sizeof(PacketMetadata), o.huge_pages);
    std::printf("arena: %zu bytes, %s\n", probe.capacity(), to_string(probe.page_mode()));
  }
  std::printf("messages/run=%llu burst=%llu queue=%zu batch=%zu runs=%d pin=%d\n",
              (unsigned long long)o.total, (unsigned long long)o.burst, o.queue_capacity,
              o.batch, runs, (int)o.pin);

  const std::uint64_t expected = expected_checksum(o.total);

  const Config configs[] = {
      {"baseline: malloc/free + mutex std::queue", run_pipeline<MallocPolicy, MutexQueue>},
      {"malloc/free + SPSC ring (ablation)", run_pipeline<MallocPolicy, SpscQueue>},
      {"arena + mutex std::queue (ablation)", run_pipeline<ArenaPolicy, MutexQueue>},
      {"arena + SPSC ring   <-- Phase 3 pipeline", run_pipeline<ArenaPolicy, SpscQueue>},
      {"arena + MPMC queue (bonus, 1P/1C)", run_pipeline<ArenaPolicy, MpmcQueue>},
  };

  std::printf("\n%-44s %14s %14s %9s  %s\n", "configuration", "median msg/s", "best msg/s",
              "speedup", "checksum");
  double base = 0;
  bool all_ok = true;
  for (const auto& c : configs) {
    Stats s = measure(c, o, runs, expected);
    if (base == 0) base = s.median;
    all_ok &= s.ok;
    std::printf("%-44s %14.0f %14.0f %8.2fx  %s\n", c.name, s.median, s.best, s.median / base,
                s.ok ? "OK" : "MISMATCH");
  }
  return all_ok ? 0 : 1;
}
