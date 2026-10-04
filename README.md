# Lock-Free Concurrency & Custom Allocators

Systems & Security SIG recruitment task (C++17, Linux). Three phases plus all three bonuses:

| Phase | What | Where |
|---|---|---|
| 1 | Arena (bump) allocator on `mmap()` | `include/arena.hpp`, `tests/test_arena.cpp` |
| 2 | Cache-conscious lock-free SPSC ring buffer | `include/spsc_queue.hpp`, `tests/test_spsc.cpp` |
| 3 | Integrated pipeline + benchmark vs `malloc` + mutex queue | `include/pipeline.hpp`, `src/pipeline_bench.cpp` |
| Bonus | MPMC queue (`compare_exchange_weak`) | `include/mpmc_queue.hpp`, `tests/test_mpmc.cpp` |
| Bonus | Huge pages for the arena (2 MiB) | `Arena(..., huge_pages=true)`, `--huge-pages` |
| Bonus | ThreadSanitizer in the build | `make tsan` |

## Build and run

```bash
make            # builds pipeline_bench and the three test binaries into build/
make test       # unit tests
make bench      # Phase 3 benchmark (defaults: 4M messages x 5 runs per config)
make tsan       # tests + short pipeline run under ThreadSanitizer
./build/pipeline_bench --help
```
Needs g++ >= 9 (C++17), make, Linux. Everything under `docs/logs/` is real output from these commands.

## Phase 1 - Arena allocator

* The pool is a single `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` region - no `malloc`/`new` anywhere on the allocation path.
* `alloc(size, alignment)` rounds `base + offset` up to the alignment (`(x + a - 1) & ~(a - 1)`, `a` must be a power of two), bounds-checks in an overflow-safe way (`size > left - pad`, never `offset + size`), then bumps the offset. It returns `nullptr` when full instead of throwing.
* **Zero internal fragmentation:** there are no per-block headers. The only padding is the alignment the caller asked for. `test_arena` proves it: 1000 allocations of 64 B @ 64 consume exactly 64 000 bytes.
* **`reset()` is O(1):** `offset_ = 0`. It does not touch the memory, which is why `create<T>()` requires trivially destructible types.
* Not thread-safe on purpose: one owner thread (the producer) means no atomics on the hot path.

## Phase 2 - SPSC ring buffer

* Bounded circular queue of pointers, power-of-two capacity, `slot = index & mask`. Indices increase monotonically and wrap naturally in unsigned arithmetic, so **all** slots are usable and `tail - head` distinguishes full from empty.
* No mutex / semaphore / condition variable. Only `std::atomic` with explicit ordering:
  * producer: write slot -> `tail_.store(release)`; consumer: `tail_.load(acquire)` -> read slot. The release/acquire pair is what makes the payload visible.
  * consumer: read slot -> `head_.store(release)`; producer: `head_.load(acquire)` before overwriting a slot. This stops the producer from reusing a slot the consumer is still reading.
  * each side reads its *own* index with `relaxed` because nobody else writes it.
* **False sharing:** `head_` and `tail_` are each `alignas(64)`, so the consumer's and producer's hot variables sit on different cache lines. Each side also keeps a private cached copy of the other's index (`head_cache_`, `tail_cache_`) on its own line and only reloads the shared atomic when the queue *looks* full/empty. `try_pop_batch` drains many items with a single release store.
* A test (`indices_on_distinct_cache_lines`) checks the layout; two-thread tests push 5M items and check order and exactly-once delivery.

## Phase 3 - Integrated pipeline

`run_pipeline<AllocPolicy, Queue>` is one producer/consumer implementation instantiated with different building blocks, so every number compares like with like.

* **Producer:** allocates a 64-byte `PacketMetadata` (exactly one cache line) from the arena only, fills it with deterministic mock data, pushes the pointer.
* **Consumer:** pops batches of 64, reads each packet and accumulates a checksum.
* **Recycling:** every `burst` messages the producer waits until `consumed == produced` (acquire load pairing with the consumer's release store of `consumed`) and then calls `arena.reset()`. The acquire/release pair guarantees every consumer read of arena memory happens-before the producer reuses it. This handshake is the subtle correctness point of the whole design.
* **Verification:** the expected checksum is recomputed single-threaded outside the timed region; every run is checked (`OK` / `MISMATCH`).
* **Baseline:** `aligned_alloc`/`free` per message plus a bounded `std::queue` under a `std::mutex` (consumer takes the lock once per batch of 64 so the baseline isn't handicapped). I also ran the two mixed configurations as an ablation to see how much each component contributes.

### Results

`docs/logs/bench_default.log` - 4,000,000 messages/run, 64 B packets, burst 65 536, queue 65 536, median of 5 runs after a warm-up:

| configuration | median msg/s | speedup |
|---|---:|---:|
| baseline: malloc/free + mutex `std::queue` | 8.70 M | 1.00x |
| malloc/free + SPSC ring | 10.51 M | 1.21x |
| arena + mutex `std::queue` | 33.93 M | 3.90x |
| **arena + SPSC ring (this project)** | **86.89 M** | **9.99x** |
| arena + MPMC queue (1P/1C) | 29.19 M | 3.36x |

All checksums matched. **Caveat:** these were measured in a sandbox with a *single* CPU core, so producer and consumer time-slice instead of running in parallel. That mostly measures per-message cost (allocation + synchronisation), not true cross-core cache-line traffic, so absolute numbers and ratios will differ on a multi-core machine. Re-run `make bench` (try `--pin`) on your own hardware and put your numbers here. Reading the table: the arena is the bigger single win (~3-4x, no allocator metadata or free-list work), the lock-free queue adds another ~2.5x on top, and the MPMC queue is slower than SPSC because of its CAS loop and per-cell sequence traffic - exactly the price of generality.

### Huge pages (bonus)

`Arena(capacity, huge_pages=true)` first tries `mmap(MAP_HUGETLB)` (explicit 2 MiB pages from the hugetlbfs pool). If the pool is empty it over-maps by 2 MiB, aligns the base to 2 MiB and calls `madvise(MADV_HUGEPAGE)` (transparent huge pages). Otherwise it silently falls back to 4 KiB pages. The mode actually obtained is printed (`arena: ... transparent huge pages (madvise)`). On the test machine THP was in `madvise` mode, so it was granted; the arena is only 4 MiB, so the TLB gain is small there (88.6 M vs 86.9 M msg/s, within noise). The benefit grows with arena size and random access patterns. To use explicit huge pages: `echo 64 | sudo tee /proc/sys/vm/nr_hugepages`.

### ThreadSanitizer (bonus)

`make tsan` builds all tests and the pipeline with `-fsanitize=thread` and runs them: **no data races reported** (`docs/logs/tsan.log`). To make sure TSan really can see this class of bug, I also ran a *negative control* - the SPSC queue with the `tail_.store(release)` weakened to `relaxed` - and TSan immediately reported a data race (`docs/logs/tsan_negative_control.log`).

### MPMC queue (bonus)

Vyukov's bounded queue: each cell has a sequence number; a thread claims a ticket with `compare_exchange_weak` on the enqueue/dequeue counter, and the payload is published through release/acquire on that cell's sequence number (the CAS itself can be `relaxed`). Tested with 4 producers x 4 consumers x 500k items: every value delivered exactly once, none lost.

## Failures and resolutions

* *Spin-waiting on a machine with fewer cores than threads.* A pure `cpu_relax()` spin loop on this 1-core sandbox would burn the whole time slice waiting for a thread that cannot run. Resolved with `Backoff`: spin ~128 iterations, then `std::this_thread::yield()`.
* *Is TSan actually checking anything?* A clean run proves little if the tool is blind, so I added the negative control above.
* *(Add your own here: compile errors, bugs you hit, things you tried that didn't help, and screenshots of your runs.)*

## Layout

```
include/   common.hpp  arena.hpp  spsc_queue.hpp  mpmc_queue.hpp  mutex_queue.hpp  pipeline.hpp
src/       pipeline_bench.cpp
tests/     test_arena.cpp  test_spsc.cpp  test_mpmc.cpp
docs/      INTERVIEW_NOTES.md   logs/
Makefile
```

## References

*C++ Concurrency in Action* (A. Williams); H. Sutter, *Atomic Weapons*; Dmitry Vyukov, 1024cores (bounded MPMC queue); `mmap(2)`, `madvise(2)`.
