# Interview prep - questions this project invites

**Arena**
- Why is bump allocation so fast? What can't it do? (no individual free, lifetime = batch)
- How do you align a pointer? Why must alignment be a power of two? Why is `offset + size > cap` an unsafe check?
- Internal vs external fragmentation: what does an arena have of each?
- Why `mmap` instead of `malloc` for the pool? What are `brk`/`sbrk`, `MAP_ANONYMOUS`, page faults, `MAP_POPULATE`?
- Where would an arena be wrong? (long-lived objects with mixed lifetimes, no destructors run on reset)

**Memory model / SPSC**
- Why release on the store and acquire on the load? What breaks with `relaxed` for both? (the consumer may see the new `tail_` but stale slot contents)
- Why is `relaxed` fine for reading your own index?
- What is false sharing? How does `alignas(64)` fix it? What is MESI cache-line ping-pong?
- Why power-of-two capacity? Why monotonically increasing indices instead of wrapping ones?
- What do the cached indices save? (cross-core cache misses on the other side's line)
- acquire/release vs `seq_cst`: what extra guarantee does seq_cst give (a single total order) and when is it needed?
- What is the ABA problem, and why does neither queue suffer from it? (sequence numbers / single owner per index)

**MPMC**
- What does `compare_exchange_weak` do on failure? Why "weak" and a loop? (spurious failure on LL/SC hardware)
- Walk through the sequence-number states of a cell. Why is the CAS allowed to be `relaxed`?
- Lock-free vs wait-free vs obstruction-free. Is this queue lock-free? (yes, system-wide progress; not wait-free)

**Pipeline**
- Why is it safe to `reset()` the arena? (consumed == produced, acquire/release handshake)
- What does a mutex cost when uncontended vs contended? (futex syscall, context switch)
- What did the ablation show about where the speedup comes from?
- How would you make the benchmark more trustworthy? (pin threads, isolate cores, more runs, report variance, `perf stat`)

**Tools**
- What does TSan instrument, and what can it NOT prove? (only races on executed paths; no proof of absence)
- Huge pages: TLB reach (4 KiB x entries vs 2 MiB x entries), explicit vs transparent.
