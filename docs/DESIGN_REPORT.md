# BufferScope — Design Report

**Question.** Under what workload and memory conditions does a database-managed buffer pool
outperform ordinary buffered I/O, and what costs does `O_DIRECT` introduce by bypassing the OS
page cache?

Every number below is a mean over ≥3 (mostly 5) repetitions from `results/raw/*.csv`. The full
tables with sd and min–max, plus plots, are in [EXPERIMENTS.md](EXPERIMENTS.md). Hardware: 2-vCPU VM, virtio disk
(~40 µs random 4 KiB read at QD1); see [ENVIRONMENT.md](ENVIRONMENT.md).

## 1. Answer in one paragraph

The custom pool **wins whenever the working set (or its hot part) fits in the memory it is given**.
A pool hit is a hash lookup plus a pin, while a page-cache hit is a `pread` syscall, a 4 KiB copy and a checksum
check. That makes the pool **2.1–2.8× faster** (E3 95/5: 195k vs 94k op/s; E5 warm: 612k vs 217k; E6 at 512 MiB:
436k vs 198k), with p99 ~5 µs vs 14–17 µs. It also **wins on skewed workloads whose hot set is larger than memory, if both
caches get the same *data* capacity**. CLOCK protected an 80/20 hot set far better than the
cgroup-limited kernel LRU (storage hit 0.428 vs 0.222, E3) and was 2.7× faster at 256 MiB for Zipf (E6). The pool
**loses** in three situations:
1. at equal *memory*, because the spec's canonical `Frame` costs 8 KiB per 4 KiB page, so the page cache holds
   twice as much data (E6: 173k vs 83k op/s at 256 MiB);
2. on cold sequential scans, where `O_DIRECT` forfeits kernel readahead (E2: 362k vs 851k op/s; 4,412 device reads vs 145);
3. whenever the OS is allowed to cache the whole dataset anyway. Then a buffered "double cache" absorbs every pool
   miss and the comparison only measures the page cache (E1, E2, E4).

## 2. When the database-managed pool wins

| Condition | Evidence | Why |
|---|---|---|
| Hot set fits in the pool | E3 95/5: 194.7k vs 94.0k op/s, p50 1.7 vs 4.6 µs; E5 (i): 612k vs 217k | No syscall, copy or checksum per access; 13 ns hit path (`micro_bench`) |
| Skewed access, hot set > memory, equal data capacity | E3 80/20: hit 0.428 vs 0.222, 26.1k vs 22.5k op/s; E6 128 MiB: 0.825 vs 0.718, 71k vs 53k op/s | CLOCK reference bits keep re-referenced pages; the kernel's cgroup-limited reclaim plus readahead of cold neighbours dilutes its cache |
| Everything fits | E6 512 MiB: 435.6k vs 198.3k op/s, p99 5.8 vs 16.5 µs | Same as row 1, with zero device I/O on either path |
| Deterministic memory use | the pool's footprint is fixed (E3/E6 `rss_anon_mib`) | No double caching and no surprise reclaim under memory pressure |

## 3. When it loses, and why

| Condition | Evidence | Why |
|---|---|---|
| Equal memory, mid-range budgets | E6 32–256 MiB: OS ahead at every point (256 MiB: 172.8k vs 83.1k op/s) | Canonical 8 KiB `Frame` = half the data per byte (finding F1). A struct-of-arrays layout would fix it |
| Cold sequential scan | E2: 851k (buffered) vs 362k (O_DIRECT) op/s | Readahead turns 4,412 synchronous 4 KiB reads into 145 ~122 KiB reads; BufferScope has no prefetcher |
| Cold start / first touch | E5 (ii): double 147k vs pool 121k op/s | Same readahead effect on first touches |
| Very small caches | E6 8 MiB: OS hit 0.464 vs 0.430 at equal capacity | The kernel's two-list (active/inactive) LRU resists one-off accesses better than plain CLOCK at tiny sizes |
| OS allowed to cache the data anyway | E1 0.25×: 202k (double) vs 22k (O_DIRECT) | The page cache is a second, free cache layer, until memory is actually constrained |

## 4. What `O_DIRECT` costs and what it buys

**Costs (measured):** every pool miss is a synchronous device read (E1 0.25×: 150k device reads, p99 157 µs vs 15.5 µs
when the page cache backs the pool). There is no readahead (E2), and cold starts are slower (E5 ii). Write-backs are synchronous
at eviction (E4: 11,890 page writes at 20% reads cut throughput from 24.1k to 14.4k op/s). It also imposes alignment rules:
4096-byte aligned buffers, offsets and lengths, otherwise the kernel returns `EINVAL`, which BufferScope surfaces as
`DirectIOAlignmentError` (tested).

**Buys:** no double caching, so a fixed memory budget is spent once. There is no system-CPU page-cache work on hits (E5 i: 0.000 s
system CPU vs 0.17 s for `os`), memory is predictable, and the database decides what stays resident.

**When the difference disappears:** if the dataset fits in RAM and is warm, buffered + pool and O_DIRECT + pool are
statistically identical (E5 i: 607k ± 57k vs 612k ± 73k op/s; E1 at ≥1×). Benchmarks that do not control the
page cache will conclude "O_DIRECT doesn't matter".

## 5. Durability cost (WAL)

Per-commit `fdatasync` costs 0.28–0.36 ms on this disk. It cuts throughput 2.3× at 80/20 and 4.4× at 20/80
(E4: 14.4k → 3.3k op/s) and pushes p99 to ~1 ms. Group commit (64 ops) recovers 3.5× at the documented risk of losing
≤63 acknowledged writes, which crash injection verifies. The I/O mode does not change the sync cost.

## 6. Design decisions that mattered

* **CLOCK over LRU.** Hit ratio within 0.9 pp of exact LRU, yet 1.6× the throughput, because LRU's O(N) victim search
  dominates CPU (E7).
* **Checksums at the I/O boundary.** CRC stamped at write-back, verified at load; in-memory frames are trusted. A
  slicing-by-8 CRC32 (2.0 µs/page vs 10.9 µs bytewise) keeps verification from dominating page-cache hits.
* **In-place updates.** Without them, overwrites grew the file and migrated hot keys onto new pages (hotspot hit ratio
  61% → 78% after the change).
* **Physiological WAL with page LSNs and full-page images.** This gives idempotent redo and torn-page repair. Crash injection at
  every protocol step recovers exactly the committed data on both I/O paths.
* **Fair comparisons.** Memory cgroups cap the page cache at the same budget as the pool. Page-cache residency is
  measured (`mincore`, cgroup `cache`), not assumed.

## 7. Limitations of the evidence

A single benchmark thread issuing synchronous I/O never saturated the device (max util 69%, average queue ≤ 1.8).
Results on miss-heavy workloads are therefore latency-bound, and a concurrent or async engine would shift absolute numbers
(the relative conclusions about cache capacity and hit-path cost should hold). The disk is virtualised. Experiment (g),
concurrency, was not run because the engine is single-threaded.
