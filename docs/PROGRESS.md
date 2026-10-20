# BufferScope — Progress Tracker

## Project Overview
**BufferScope**: OS-Aware Database Buffer Manager  
**Core Research Question**: Under what workload and memory conditions does a
database-managed buffer pool outperform ordinary buffered I/O, and what costs
does O_DIRECT introduce by bypassing the OS page cache?

---

## Phase Status

| Phase | Title | Status | Started | Completed |
|-------|-------|--------|---------|-----------|
| 0 | Disk Page Library | 🔄 IN PROGRESS | 2026-10-19 | - |
| 1 | Minimal KV Store | ⬜ NOT STARTED | - | - |
| 2 | Buffer Pool | ⬜ NOT STARTED | - | - |
| 3 | CLOCK Replacement & Dirty-Page Flushing | ⬜ NOT STARTED | - | - |
| 4 | Buffered I/O Baseline & Benchmark Harness | ⬜ NOT STARTED | - | - |
| 5 | Direct I/O Path | ⬜ NOT STARTED | - | - |
| 6 | WAL + Recovery | ⬜ NOT STARTED | - | - |
| 7 | Workload Framework | ⬜ NOT STARTED | - | - |
| 8 | Comparative Experiments | ⬜ NOT STARTED | - | - |
| 9 | Polish & Freeze | ⬜ NOT STARTED | - | - |

---

## Phase 0: Disk Page Library

### Deliverable
4 KiB pages, deterministic serialization, checksum, and correct file offsets;
read-after-write and restart tests pass.

### Exit Criterion
Read-after-write and restart tests pass.

### Checklist
- [x] Environment verified and documented in `docs/ENVIRONMENT.md`
- [x] Repository scaffolded with canonical layout
- [x] `include/bufferscope/page.hpp` — Page abstraction with header
- [x] `src/page.cpp` — Deterministic serialization, CRC32 checksum
- [x] `include/bufferscope/file_manager.hpp` — File I/O interface
- [x] `src/file_manager.cpp` — POSIX pread/pwrite/fdatasync
- [x] `tests/page_test.cpp` — Page unit tests
- [x] `tests/file_manager_test.cpp` — FileManager unit tests
- [ ] All tests pass (build + run)
- [ ] Phase 0 exit criterion verified

### Files Created
- `include/bufferscope/page.hpp`
- `include/bufferscope/file_manager.hpp`
- `src/page.cpp`
- `src/file_manager.cpp`
- `src/main.cpp` (stub)
- `tests/page_test.cpp`
- `tests/file_manager_test.cpp`
- `docs/ENVIRONMENT.md`
- `CMakeLists.txt`
- `.gitignore`

### Design Decisions
1. **Checksum**: CRC32 (ISO 3309, polynomial 0xEDB88320). Chosen for simplicity
   and adequate error detection at page level. Not cryptographic.
2. **Page layout**: 32-byte header followed by slot directory (growing forward)
   and record data (growing forward after slots). Records use fixed-length-with-
   length-prefix format: `[key_len:2][val_len:2][key_data][val_data]`.
3. **Deletion**: Tombstone-based (slot length zeroed). No page compaction yet.
4. **File I/O**: Standard POSIX open/pread/pwrite/fdatasync. O_DIRECT deferred
   to Phase 5 as specified.

---

## Definition of Done Tracking

| Item | Status |
|------|--------|
| Persistent page-oriented KV store works correctly across restarts | ⬜ Phase 1 |
| Custom buffer pool supports fetch, pin/unpin, dirty tracking, and eviction | ⬜ Phase 2-3 |
| CLOCK replacement behaves correctly under controlled tests | ⬜ Phase 3 |
| Buffered POSIX and O_DIRECT paths both work with the same logical workload | ⬜ Phase 5 |
| Benchmark suite reports cache hit ratio, throughput, p50/p99 latency, bytes moved, and flush counts | ⬜ Phase 4-8 |
| At least five comparative experiments are reproducible | ⬜ Phase 8 |
| Crash injection validates WAL/recovery semantics | ⬜ Phase 6 |
| README explains when the custom buffer manager wins, loses, and why | ⬜ Phase 9 |
