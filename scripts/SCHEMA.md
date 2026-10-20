# BufferScope result schemas

All benchmark output is produced by `build/bench_runner` from counters inside the
engine (`CacheMetrics`, `IOStats`, `WalStats`) and from the OS (`getrusage`,
`/proc/self/io`, `mincore`). Nothing is typed in by hand.

## 1. Summary CSV (`--summary=PATH`, appended, one row per run)

| Column | Unit | Meaning |
|---|---|---|
| `label` | – | free-form experiment label (`--label`) |
| `run` | – | 1-based repetition index |
| `pattern` | – | `sequential` \| `uniform` \| `zipf` \| `hotspot` |
| `read_pct`, `delete_pct` | % | GET share of ops; DELETE share of writes |
| `keys`, `ops`, `value_size` | count / count / bytes | key space, measured ops, value bytes |
| `zipf_theta`, `hot_keys`, `hot_ops`, `scramble` | – | pattern parameters (0 when not applicable) |
| `seed` | – | workload seed |
| `pool_frames`, `pool_mib` | frames / MiB | buffer-pool capacity (4 KiB frames) |
| `file_pages`, `data_mib` | pages / MiB | data-file size after the load phase |
| `io_mode` | – | `buffered` (page cache) \| `direct` (O_DIRECT) |
| `durability` | – | `off` \| `sync` \| `group` |
| `replacer` | – | `clock` \| `lru` |
| `cache_state` | – | `warm` (pool+OS warm), `oswarm` (pool cold, OS warm), `cold` (both cold) |
| `os_cache_resident_before` | fraction | share of the data file resident in the OS page cache right before the measured phase (`mincore`) |
| `elapsed_s` | s | wall time of the measured op loop |
| `throughput_ops` | ops/s | `ops / elapsed_s` |
| `p50_us` … `p999_us`, `mean_us`, `max_us` | µs | per-op latency distribution (steady_clock, every op timed) |
| `gets`, `puts`, `deletes` | count | op mix actually executed |
| `hit_ratio`, `hits`, `misses` | – | buffer-pool page lookups |
| `evictions`, `dirty_evictions` | count | victims chosen; victims that needed write-back |
| `page_flushes` | count | dirty pages written back (evictions + explicit flushes) |
| `bytes_read`, `bytes_written` | bytes | pages moved between pool and FileManager |
| `page_reads`, `page_writes` | count | `pread`/`pwrite` calls issued by FileManager |
| `io_iops` | IO/s | `(page_reads + page_writes) / elapsed_s` — page I/O rate the engine issued |
| `data_fsyncs` | count | `fdatasync` on the data file |
| `wal_syncs`, `wal_bytes`, `wal_sync_ms` | count / bytes / ms | WAL flushes, bytes appended, time inside WAL fdatasync |
| `end_flush_ms` | ms | final checkpoint after the loop (deferred write cost, **not** in `elapsed_s`) |
| `cpu_user_s`, `cpu_sys_s`, `cpu_util` | s / s / cores | process CPU during the loop; `cpu_util` ≈ 1.0 means CPU-bound single thread |
| `minor_faults`, `major_faults` | count | page faults during the loop |
| `ctx_voluntary`, `ctx_involuntary` | count | context switches (voluntary ≈ blocking on I/O) |
| `os_read_bytes`, `os_write_bytes` | bytes | `/proc/self/io` storage-layer bytes: reads actually served by the device (page-cache misses), writes submitted for writeback |
| `syscalls_read`, `syscalls_write` | count | read/write syscalls (`/proc/self/io`) |

Interpretation helpers:
* `bytes_read > 0` with `os_read_bytes ≈ 0` → buffer-pool misses were served by the OS page cache.
* `os_read_bytes ≈ bytes_read` → misses went to the device (cold cache or O_DIRECT).
* `cpu_util ≈ 1` and low `ctx_voluntary` → software-bound; high `ctx_voluntary` → I/O-bound.

## 2. JSON (`--json=PATH`)

```json
{ "runs": [ { <same keys as the CSV row> }, ... ],
  "aggregate": { "<numeric column>": {"mean": x, "sd": x, "min": x, "max": x}, ... } }
```

## 3. Per-operation trace (`--trace=PATH`; `_runN` suffix when `--runs > 1`)

`seq,op,key,latency_ns` — `seq` starts at 0 and increases by 1 per measured
operation; `op` ∈ {GET, PUT, DELETE}; `key` is the fixed-width key (`k%010u`).

## 4. Operation dump (`--dump-ops=PATH`, no execution)

`seq,op,key,value_len,value_crc32` — the exact generated sequence. Two invocations
with the same parameters produce byte-identical files (tested by `workload_test`
and the `bench_runner_smoke` CTest).

## 5. Determinism guarantees

* The PRNG (SplitMix64) and all distributions are implemented in `bench/workload.cpp`;
  no `std::*_distribution` is used (their output is implementation-defined).
* `sequential`, `uniform` and `hotspot` use integer arithmetic only, so they are
  identical on every platform (golden hashes frozen in `tests/workload_test.cpp`).
* `zipf` uses `std::pow`; it is identical across runs on one platform and libm
  version, which is what reproducing an experiment requires.
* Values depend only on `(seed, seq, key_id)`.
