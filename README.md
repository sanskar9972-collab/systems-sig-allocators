# Lock-Free Concurrency and Custom Allocators

Task for the Systems and Security SIG recruitment. Written in C++17 and tested on Linux (WSL2, Ubuntu).

## What I did

- Phase 1: arena allocator using `mmap`
- Phase 2: lock-free SPSC ring buffer (no mutex)
- Phase 3: producer/consumer pipeline using the arena and the queue, and a benchmark against `malloc` + a mutex queue
- Bonus: MPMC queue, huge pages for the arena, ThreadSanitizer

## How to build and run

```
make          # build everything
make test     # run the tests
make bench    # run the benchmark
make tsan     # run tests with ThreadSanitizer
```

You need g++, make and Linux. It uses `mmap` and `pthread` so it does not build on plain Windows. I used WSL2.

## Phase 1: Arena allocator (`include/arena.hpp`)

- It gets one big block of memory from the OS with `mmap`.
- `alloc(size, alignment)` rounds the current pointer up to the alignment and then moves the pointer forward. It returns `nullptr` if there is no space left.
- There are no headers per allocation, so no memory is wasted except the alignment padding.
- `reset()` just sets the offset back to 0, so freeing everything is O(1).
- It is not thread safe on purpose. Only the producer thread uses it.

Tests (`tests/test_arena.cpp`) check alignment, that blocks do not overlap, running out of space, and that `reset()` works.

## Phase 2: SPSC queue (`include/spsc_queue.hpp`)

- It is a ring buffer with a fixed size (power of two). One thread pushes and one thread pops.
- No mutex. Only `std::atomic`.
- The producer writes the item and then updates `tail` with `memory_order_release`. The consumer reads `tail` with `memory_order_acquire` and then reads the item. This way the consumer always sees the full item.
- It works the same the other way round for `head`, so the producer never overwrites an item the consumer is still reading.
- `head` and `tail` are on different cache lines (`alignas(64)`) so the two threads do not slow each other down (false sharing).
- Each side keeps a local copy of the other side's index and only reads the real one when the queue looks full or empty.

Tests (`tests/test_spsc.cpp`) check the order of items, full/empty, wrap-around, and push 5 million items between two threads.

## Phase 3: Pipeline and benchmark (`include/pipeline.hpp`, `src/pipeline_bench.cpp`)

- The producer makes fake packets (64 bytes each) in the arena and pushes the pointers into the queue.
- The consumer pops them and adds up a checksum.
- After every 65536 packets the producer waits until the consumer has finished all of them and then calls `reset()` on the arena.
- At the end I compare the checksum with one calculated separately, to make sure nothing was lost or changed.
- For the baseline I used `malloc`/`free` for every packet and a `std::queue` with a `std::mutex`.
- I also tried the mixed versions (arena with mutex queue, and malloc with SPSC queue) to see which part helps more.

### Results

4 million packets per run, median of 5 runs. All checksums were OK.

| Version | Messages per second | Speedup |
|---|---:|---:|
| malloc + mutex queue (baseline) | 2,535,190 | 1.00x |
| malloc + SPSC queue | 4,362,047 | 1.72x |
| arena + mutex queue | 9,694,757 | 3.82x |
| **arena + SPSC queue** | **84,188,345** | **33.21x** |
| arena + MPMC queue | 61,660,439 | 24.32x |

![benchmark](docs/screenshots/02_benchmark_and_tsan_error.png)

What I understood from this:
- Most of the gain comes from the arena, because there is no malloc/free for each packet.
- The lock-free queue helps a lot more once malloc is gone, so together they give 33x.
- MPMC is slower than SPSC because it has to use compare-and-swap, since many threads can use it.
- The numbers change a bit from run to run on my laptop (best run was 123 million), so I used the median.

## Bonus

**MPMC queue (`include/mpmc_queue.hpp`)**: bounded queue where many producers and consumers can work at the same time. Each slot has a sequence number and threads take a ticket using `compare_exchange_weak`. I tested it with 4 producers and 4 consumers and every item came out exactly once.

**Huge pages**: `Arena(size, true)` first tries `MAP_HUGETLB`. If that does not work it uses `madvise(MADV_HUGEPAGE)`, and if that also fails it uses normal pages. On my machine the test printed that transparent huge pages were granted. The benchmark prints which mode it got. Use `--huge-pages` to turn it on.

**ThreadSanitizer**: `make tsan` runs the tests and the pipeline with `-fsanitize=thread`. No data races were reported. I also changed one `release` to `relaxed` in a copy of the queue to check that TSan really catches the bug, and it did (`docs/logs/tsan_negative_control.log`).

![tsan](docs/screenshots/03_tsan_clean_after_fix.png)

## Problems I faced and how I fixed them

1. **`make` and `unzip` not found in PowerShell.** The code uses Linux functions, so I used WSL2 (Ubuntu) and built it there.
2. **`make tsan` crashed with `FATAL: ThreadSanitizer: unexpected memory mapping`.** This happens with GCC 13's TSan on newer kernels because of address randomization. I fixed it with `sudo sysctl vm.mmap_rnd_bits=28` and then it ran fine. (The error is visible at the bottom of the benchmark screenshot.)
3. **`git push` said "Password authentication is not supported".** GitHub needs a personal access token instead of the password. I made a token and used that. Git also asked for my name and email first.
4. **Threads waiting in a loop when there are fewer cores than threads.** In a 1-core test environment the waiting thread wasted its whole time slice. I added a small `Backoff` that spins a few times and then calls `yield()`.

## Screenshots

Build and tests:

![build and tests](docs/screenshots/01_build_and_tests.png)

Benchmark (with the TSan error before the fix) and the clean TSan run are shown above.

## Files

```
include/   arena.hpp  spsc_queue.hpp  mpmc_queue.hpp  mutex_queue.hpp  pipeline.hpp  common.hpp
src/       pipeline_bench.cpp
tests/     test_arena.cpp  test_spsc.cpp  test_mpmc.cpp
docs/      screenshots/  logs/  INTERVIEW_NOTES.md
Makefile
```

The logs in `docs/logs/` are from an earlier run on a 1-core Linux container, so the numbers there are lower than in the table above.

## References

- C++ Concurrency in Action by Anthony Williams
- Dmitry Vyukov, bounded MPMC queue (1024cores)
- man pages for `mmap` and `madvise`
