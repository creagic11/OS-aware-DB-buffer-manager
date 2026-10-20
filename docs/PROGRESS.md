# BufferScope — Project Tracking & Milestone Progress

**Project**: BufferScope — OS-Aware Database Buffer Manager
**Repository**: https://github.com/creagic11/OS-aware-DB-buffer-manager
**Core goal**: a disk-backed key-value store in C++20 used to study Linux page-cache behaviour vs. a custom buffer manager vs. an O_DIRECT path.

---

## Phase Roadmap & Status

| Phase | Title | Status | Exit criterion | Evidence |
|---|---|---|---|---|
| 0 | Disk Page Library | ✅ Done (reviewed) | Read-after-write & restart tests pass | `page_test`, `file_manager_test` |
| 1 | Minimal KV Store | ✅ Done (reviewed) | 10,000 randomized ops match an oracle | `kv_correctness_test` |
| 2 | Buffer Pool | ✅ Done (reviewed, test strengthened) | Hits/misses match a reference model | `buffer_pool_test`, `buffer_pool_reference_test` (72 sequences × 2 policies) |
| 3 | CLOCK Replacement & Dirty Flushing | ✅ Done (reviewed) | Hot/cold, full-pool, all-pinned, dirty-flush tests | `clock_replacer_test`, `buffer_pool_eviction_test`, `buffer_pool_reference_test` |
| 4 | Buffered Baseline & Bench Harness | ✅ Done (reviewed, runner fixed) | Stable baseline | `bench_runner` (5 runs, CoV reported), `micro_bench` |
| 5 | Direct I/O Path | ✅ Done (reviewed, metrics wired) | Correctness identical on both paths; alignment errors diagnosable | `direct_io_test`, `io_mode_correctness_test` |
| 6 | WAL + Recovery | ⏳ Next | Injected crashes recover committed data | — |
| 7 | Workload Framework | ⬜ Pending | Deterministic, machine-readable workloads | — |
| 8 | Comparative Experiments | ⬜ Pending | ≥ 5 experiments with p50/p99 + IOPS | — |
| 9 | Polish & Freeze | ⬜ Pending | Fresh clone runs the suite with one command | — |

---

## Phase 0–5 Review (independent verification before Phase 6)

**Source of truth.** The original working tree lived in WSL (`~/bufferscope`) and was
not reachable for the review. It was reconstructed byte-for-byte by replaying the
project's generator scripts in their original order (`scaffold.py`, `fix_ambiguity.py`,
`update_page.py`, `create_phase1..5.py`, `fix_kv_checksum.py`); the first six commits
after the repository wipe are exactly that replay, one commit per phase. The
original commit hashes recorded earlier (`2c3a148`, `b480368`, `e46036c`, `a7b20de`,
`8c35e64`, `1adede8`) belong to the local WSL repository and do not exist on GitHub.

**Result.** The replayed tree built cleanly and its 41 tests passed, but the review
found defects that the existing tests did not exercise. All are fixed in the
"Phase 0–5 review" commit, each with a regression test.

| # | Area | Defect | Impact | Fix |
|---|---|---|---|---|
| 1 | Metrics (Phase 4/5) | `record_bytes_read/written` existed but were **never called**; no flush/eviction counters | "bytes moved" and "flush counts" (Definition of Done) were always 0 | Counters wired in the buffer pool + independent `IOStats` in `FileManager`; tests cross-check both layers |
| 2 | BufferPool | Failed page load (I/O error / corruption) left the victim frame neither mapped nor free | Pool permanently shrinks; eventually "all pinned" | Frame returned to free list on any load failure |
| 3 | BufferPool | Pages loaded from disk were **not** checksum-verified by the pool | Corruption only caught where the KV layer happened to check | Pool verifies magic + CRC32 + page id on every load (`ChecksumMismatchError`) |
| 4 | BufferPool | Fetching past EOF then evicting created sparse-file **holes** that later read back as "corrupt" pages | Spurious checksum failures | No-holes invariant: gaps are filled on write-back |
| 5 | BufferPool | `new_page()` used `page_count()` and could hand out an id already materialised in the pool | Two frames for one page | Pool tracks `num_pages_` |
| 6 | KVStore | Fallback paths read/wrote pages **directly via FileManager** when a fetch failed | A stale dirty frame could later overwrite newer data | All access goes through the pool; exhaustion raises `BufferPoolExhaustedError` |
| 7 | KVStore | Defaulted move constructor while `BufferPoolManager` holds `FileManager&` | Dangling reference after a move | Move deleted |
| 8 | Page/KVStore | Record size computed in `uint16_t` | A 65,540-byte record wrapped to 4 bytes and was "accepted" (truncated) | Size checked in `size_t` against `MAX_KV_BYTES` |
| 9 | FileManager | `errno` read after short (non-negative) transfers; no `EINTR`/partial handling | Misleading error messages | Full-transfer loops, typed errors (`DirectIOAlignmentError`, `PageNotFoundError`, …) |
| 10 | Bench runner | All runs reused one growing DB file; warm-up ran on a store that was then closed | Runs not independent; "warm" runs started cold | Fresh file per run, warm-up on the same instance, metrics reset before recording |
| 11 | Phase 2 test | Reference model only covered a pool that never evicts | Exit criterion only weakly met | 72 scripted sequences × {CLOCK, LRU} vs. an independent model, incl. pins & eviction |
| 12 | Phase 5 test | Only a few KV tests ran under O_DIRECT | Spec asks for the full suite on both paths | Oracle, restart, corruption, frame-leak and metrics tests parameterised over both modes |
| 13 | Phase 4 | Google Benchmark listed in spec but unused | — | `bench/micro_bench.cpp` (CRC32, page insert, pool hit, KV get) |
| 14 | Hot path | CRC32 of the whole page computed on every PUT and verified on every GET of an in-memory frame | ~4 KiB CRC per op inflated latency | Checksums are stamped at write-back and verified at load (standard practice) |

Note on #14: the Phase 4 baseline recorded on the WSL machine (58,045 op/s, p50 12.46 µs)
was measured *with* per-op CRC. It is kept as a historical figure for that code
version; it is not comparable with post-review numbers.

**Verification after fixes (secondary environment, see `docs/ENVIRONMENT.md`):**
- 9 test executables, all passing in Debug with **AddressSanitizer + UBSan** and in Release.
- Baseline (`bench_runner --ops 50000 --keys 1000 --runs 5`, Release):
  buffered 1,175,463 ± 90,871 op/s (CoV 7.7 %), p99 13.92 ± 0.44 µs;
  direct 542,643 ± 25,252 op/s (CoV 4.7 %), p99 29.72 ± 6.36 µs.
  "Stable enough" for this 2-vCPU VM is CoV < 10 % across 5 runs.

---

## Phase Log

### Phase 0 — Disk Page Library ✅
4096-byte slotted page (32-byte header: magic, page_id, LSN, record_count,
free_space_offset, checksum), deterministic serialization, table-driven CRC32,
POSIX `pread`/`pwrite`/`fdatasync` FileManager at `page_id * 4096`.

### Phase 1 — Minimal KV Store ✅
`KVIndex` (hash map key → page/slot), tombstoning updates, startup full-file scan,
10,000 randomized ops vs. `std::unordered_map` oracle, restart and corruption tests.

### Phase 2 — Buffer Pool ✅
Canonical `Frame` (`alignas(4096)` data), page table, pin/unpin, dirty tracking,
hit/miss metrics; KV rewired through the pool.

### Phase 3 — CLOCK & Dirty Flushing ✅
`ClockReplacer` per spec pseudocode (2N sweep bound), `LRUReplacer` baseline,
flush-before-reuse, all-pinned returns `nullptr` without evicting.

### Phase 4 — Buffered Baseline & Harness ✅
`bench/workload.*`, `bench/runner.cpp`, `LatencyTracker` (p50/p90/p99/p99.9),
`bench/micro_bench.cpp` (Google Benchmark).

### Phase 5 — Direct I/O ✅
`IOMode::DIRECT` (`O_DIRECT`), 4096-byte aligned transfers, `DirectIOAlignmentError`
for kernel `EINVAL`, identical metrics on both paths, `--io-mode=buffered|direct`
on `bench_runner` and `bufferscope`.
