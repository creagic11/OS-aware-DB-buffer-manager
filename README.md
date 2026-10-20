# BufferScope — OS-Aware Database Buffer Manager

**BufferScope** is a small persistent key-value store (PUT / GET / DELETE) in C++20.
Its storage layer can run through ordinary buffered POSIX I/O (Linux page cache), a
custom database-managed buffer pool with CLOCK replacement, or the same pool over
`O_DIRECT`. Controlled workloads then measure cache hits, replacement, write-back and
tail latency on each path.

> **Core research question.** Under what workload and memory conditions does a
> database-managed buffer pool outperform ordinary buffered I/O, and what costs does
> `O_DIRECT` introduce by bypassing the OS page cache?
>
> The short answer, backed by the numbers in [§ Results](#results), is in
> [docs/DESIGN_REPORT.md](docs/DESIGN_REPORT.md).

---

## Architecture

```
            ┌──────────────────────────── KVStore (PUT/GET/DELETE) ────────────────────────────┐
 client ──► │ KVIndex (hash: key → page/slot)        LogManager (WAL)      RecoveryManager       │
            └───────────────┬──────────────────────────────┬───────────────────────────────────┘
                            │ fetch / unpin / new page     │ append, flush_to(page LSN)
                            ▼                              │  (write-ahead rule)
            ┌──────── BufferPoolManager ────────┐◄──────────┘
            │ frames[N]  page table  free list  │  CLOCK (primary) / LRU (baseline) replacer
            │ pin_count dirty referenced valid  │  dirty victim written back before reuse
            └───────────────┬───────────────────┘
                            │ read_raw_aligned / write_raw_aligned (4 KiB, aligned)
                            ▼
            ┌──────── FileManager (IOStats) ────┐
            │ BUFFERED: pread/pwrite ─► Linux page cache ─► block layer ─► SSD
            │ DIRECT:   pread/pwrite with O_DIRECT ───────► block layer ─► SSD
            └────────────────────────────────────┘
 data file: page_id × 4096         WAL file: <db>.wal (buffered + fdatasync)
```

The three configurations compared in the experiments:

| Name | I/O mode | Buffer pool | Who caches |
|---|---|---|---|
| **os** | buffered | tiny (16 frames) | Linux page cache |
| **pool** | `O_DIRECT` | sized to the memory budget | the database only |
| **double** | buffered | sized to the budget / share of it | both (double buffering) |

### Page format (4096 bytes, slotted)

```
0        4         8                16      18       20        24          32
┌────────┬─────────┬────────────────┬───────┬────────┬─────────┬───────────┐
│ magic  │ page_id │ LSN (u64)      │ count │ free   │ CRC32   │ reserved  │  header (32 B)
├────────┴─────────┴────────────────┴───────┴────────┴─────────┴───────────┤
│ slot 0 (off,len) │ slot 1 │ … grows →                                    │
│                         free space                                         │
│                                          … ← records grow │ rec1 │ rec0  │
└────────────────────────────────────────────────────────────────────────────┘
record = [key_len u16][val_len u16][key][value]; deleted slot = (0,0)
```

* CRC32 (ISO 3309, slicing-by-8) over the page with the checksum field zeroed;
  stamped at write-back, verified on every load from disk.
* Page LSN = LSN of the last WAL record applied; this makes redo idempotent.
* Overwrites that fit are done in place; larger values are tombstoned and re-appended.

### Durability (Phase 6)

Physiological redo WAL (`PUT/UPDATE/TOMBSTONE (page, slot)`, `PAGE_IMAGE`,
`CHECKPOINT`), CRC-protected records, COMMIT flag per operation, page-LSN-idempotent
redo. A full-page before-image is logged on the first touch after a checkpoint, which
lets recovery repair torn pages. Atomic checkpoints use temp file + fsync + rename.
Durability modes: `sync` (fdatasync per mutation, default), `group` (every N), `off`.
Crash injection (fork + SIGKILL at named points) proves recovery returns exactly the
committed data. See [docs/PROGRESS.md](docs/PROGRESS.md#phase-6--wal--recovery-).

---

## Build from a clean clone

Requirements: Linux, GCC ≥ 11 or Clang ≥ 14, CMake ≥ 3.20, Ninja, GoogleTest,
Google Benchmark (optional), Python ≥ 3.11 with pandas + matplotlib; `sysstat` and
`perf` are optional (raw iostat/perf logs).

```bash
sudo apt install build-essential cmake ninja-build libgtest-dev libbenchmark-dev \
                 python3-pandas python3-matplotlib sysstat
git clone https://github.com/creagic11/OS-aware-DB-buffer-manager.git
cd OS-aware-DB-buffer-manager
scripts/run_all.sh            # build → tests → full benchmark matrix → plots → validation
```

`scripts/run_all.sh --quick` runs the identical pipeline with 10× smaller workloads
(about 2 minutes, a smoke check, **not** for reporting). `--no-bench` stops after the tests.
Put the data directory on a real block-device filesystem: `DATA_DIR=/path/on/ext4
scripts/run_all.sh`. Running as root (or with cgroup delegation) enables the
equal-memory-budget experiments; without it those configurations are skipped and
reported as skipped.

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure              # 13 suites
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUFFERSCOPE_SANITIZE=ON
```

## Using the KV store

```bash
build/bufferscope --db data/demo.db put user:1 alice
build/bufferscope --db data/demo.db get user:1            # alice
build/bufferscope --db data/demo.db --io-mode=direct --pool-mb=64 stats
build/bufferscope --config=configs/example.conf --db data/demo.db get user:1
```

Flags: `--io-mode=buffered|direct`, `--pool N` / `--pool-mb M`,
`--durability=sync|group|off`, `--group-commit N`, `--config FILE`.
Commands: `put`, `get`, `del`, `checkpoint`, `stats` (prints the recovery report).

## Running benchmarks

```bash
# one configuration, 5 repetitions, machine-readable output
build/bench_runner --pattern=hotspot --hot-keys=0.2 --hot-ops=0.8 --mix=80/20 \
    --keys=600000 --ops=150000 --pool-mb=16 --io-mode=direct --cache=cold --runs=5 \
    --summary=out.csv --json=out.json --trace=ops.csv

build/bench_runner --config=configs/example.conf --runs=3     # file + overrides

# the full comparative matrix (configs/experiments.toml) + plots + validation
python3 scripts/run_experiments.py && python3 scripts/plot.py && python3 scripts/check_results.py
```

Every parameter is a runtime flag or config key; nothing is recompiled. Output
columns are documented in [scripts/SCHEMA.md](scripts/SCHEMA.md). Each run reports the
cache state it was measured in (`warm` / `oswarm` / `cold`) and the measured OS
page-cache residency of the data file (`mincore`).

---

## Results

Measured on a 2-vCPU cloud VM with a virtio disk, 3–5 repetitions per point (full tables with ±sd and min–max:
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md); answer to the research question: [docs/DESIGN_REPORT.md](docs/DESIGN_REPORT.md)).

| Experiment | Result |
|---|---|
| (a) Working-set sweep | Pool hit ratio = min(1, pool/WS) (0.249 / 0.499 / 1.00). Below 1×, O_DIRECT is device-bound (22k op/s at 0.25×), while buffered + pool gets 202k op/s because the page cache absorbs the misses |
| (b) Sequential vs random, cold | Readahead: 145 device reads instead of 4,412 for a scan, so buffered is 2.3× faster than O_DIRECT (851k vs 362k op/s) |
| (c) Hotspot, 10 MiB budget | Equal data capacity, 80/20: CLOCK storage-hit 0.428 vs OS page cache 0.222. 95/5 (fits): pool 195k vs OS 94k op/s, p99 80 vs 136 µs |
| (d) Read/write + WAL | fdatasync per commit (0.28–0.36 ms) costs 2.3–4.4× throughput; group commit (64) recovers 3.5× |
| (e) I/O mode | Data fits and is warm: buffered and O_DIRECT are indistinguishable (607k vs 612k op/s). 256 MiB data with a 64 MiB budget: the OS wins at equal memory (44.6k vs 36.9k op/s) |
| (f) Budget sweep 8–512 MiB | Equal memory: OS wins 32–256 MiB (canonical 8 KiB frames), pool wins 2.2× once everything fits. Equal data capacity: pool ≥ OS from 64 MiB, 2.7× at 256 MiB |
| extra: CLOCK vs LRU | Hit ratio within 0.9 pp; CLOCK 1.6× faster (LRU's O(N) victim scan) |

![Memory-budget sweep](results/plots/e6_memory_budget.png)

**When the custom buffer manager wins:** when the hot set fits in its memory, a pool hit (hash lookup + pin) is 2–2.8× cheaper
than a page-cache hit (syscall + copy + checksum). For skewed workloads larger than memory, CLOCK at equal data capacity
keeps the hot set better than the cgroup-limited kernel LRU.
**When it loses:** at equal memory with the spec's canonical 8 KiB `Frame` (half the data per byte), on cold sequential scans
(no readahead with O_DIRECT), and whenever the OS may cache the dataset anyway (double caching absorbs pool misses).
**What O_DIRECT costs:** synchronous device reads on every miss, no readahead, synchronous write-back at eviction, and
4 KiB alignment discipline. **What it buys:** no double caching, zero page-cache system CPU, and predictable memory.

---

## Repository layout

```
include/bufferscope/   page, file_manager, buffer_pool, replacer, metrics, kv_index,
                       kv_store, wal, recovery, crash, errors, config_file
src/                   implementations + main.cpp (KV CLI)
bench/                 workload generator, bench_runner, Google Benchmark micro_bench
tests/                 13 CTest suites (unit, reference-model, both-I/O-mode, crash injection)
configs/               experiments.toml (benchmark matrix), example.conf
scripts/               run_all.sh, run_experiments.py, plot.py, check_results.py, SCHEMA.md
results/               raw/ CSV + iostat/perf logs, plots/, summary/ (generated)
docs/                  DESIGN_REPORT.md, EXPERIMENTS.md, PROGRESS.md, STUDY.md, ENVIRONMENT.md
```

## Limitations

* **Single-threaded engine.** No latching, and the KV store is not thread-safe. The
  optional concurrency experiment (g) is therefore not run.
* **Space is not reclaimed.** A tombstoned or relocated record keeps its slot and
  bytes, and there is no compaction or vacuum. Fixed-size overwrites are in place, so
  the benchmark workloads do not grow the file.
* **In-memory hash index.** It is rebuilt by a full scan at startup and costs about
  70 B per key of RAM. Nothing is ordered (no range scans).
* **Frame layout.** The spec's canonical `Frame` (metadata + `alignas(4096)` data)
  occupies 8 KiB per 4 KiB page. Memory-budgeted experiments account for this
  honestly; a struct-of-arrays layout would halve the pool's memory cost.
* **WAL.** A corrupt record in the middle of the log ends the log (same as
  PostgreSQL). The WAL itself always uses buffered I/O + `fdatasync`.
* **No mmap path.** mmap is out of scope per the spec.
* **Measurement environment.** Reported numbers come from a 2-vCPU cloud VM with a
  virtio disk (see `docs/ENVIRONMENT.md` and `results/raw/environment.txt`).
  Absolute values will differ on other hardware; rerun `scripts/run_all.sh` to
  reproduce on yours.
