# BufferScope: Comprehensive Study Guide & Engineering Notes

> **Target Audience**: Anyone curious about how database storage engines, operating system memory management, and file systems interact — written from absolute scratch to advanced systems engineering details.

---

## Table of Contents
1. [Core Foundations: Why Do Databases Manage Storage?](#1-core-foundations-why-do-databases-manage-storage)
   - 1.1 The Hardware Memory Hierarchy & Latency Numbers
   - 1.2 Volatility vs. Durability
   - 1.3 Why Can't We Just Use Virtual Memory / `mmap`?
2. [Phase 0 Deep-Dive: The Anatomy of a Database Disk Page](#2-phase-0-deep-dive-the-anatomy-of-a-database-disk-page)
   - 2.1 Why 4096 Bytes (4 KiB)?
   - 2.2 The Slotted Page Architecture
   - 2.3 The Page Header and Integrity Verification
   - 2.4 CRC32: Mathematical Basis & Fast Lookup Table Implementation
   - 2.5 Deterministic Serialization: Why Bit-for-Bit Identity Matters
3. [Operating System File I/O Mechanics](#3-operating-system-file-io-mechanics)
   - 3.1 The Linux Page Cache (VFS, Dirty Pages, Writeback)
   - 3.2 POSIX File APIs: `pread` & `pwrite` vs. `lseek`
   - 3.3 Durability Barriers: `fsync` vs. `fdatasync`
   - 3.4 The Dual-Buffering Problem & `O_DIRECT` Preview
4. [Phase 1 Deep-Dive: Building a Minimal Persistent Key-Value Store](#4-phase-1-deep-dive-building-a-minimal-persistent-key-value-store)
   - 4.1 From Flat Pages to a Key-Value Store
   - 4.2 Indexing Strategies: Hash Index vs. B-Tree vs. LSM-Tree
   - 4.3 Handling Updates: In-Place Modification vs. Tombstoning
   - 4.4 Startup Recovery via Full-File Page Scans
   - 4.5 Oracle-Based Verification & Fuzz Testing (10,000 Operations)
5. [Phase 2 Deep-Dive: The Database Buffer Pool Manager](#5-phase-2-deep-dive-the-database-buffer-pool-manager)
   - 5.1 What is a Buffer Pool?
   - 5.2 Pages vs. Frames: Logical Blocks vs. Physical Memory
   - 5.3 The Canonical Frame Structure & Memory Alignment (`alignas(4096)`)
   - 5.4 The Page Table: Mapping Disk Blocks to Memory Frames
   - 5.5 The Pin / Unpin Protocol: Preventing Premature Eviction
   - 5.6 Metrics: Atomic Counters, Hit Ratio, and Cache Warmth
6. [Phase 3 Deep-Dive: CLOCK Replacement & Dirty-Page Flushing](#6-phase-3-deep-dive-clock-replacement--dirty-page-flushing)
   - 6.1 Why Page Replacement is the Core of Storage Performance
   - 6.2 Comparison of Algorithms: Optimal, FIFO, LRU, and CLOCK
   - 6.3 Why Production Databases Reject Pure LRU (Lock Contention)
   - 6.4 The CLOCK (Second Chance) Algorithm Step-by-Step
   - 6.5 The "All Frames Pinned" Failure Mode & Deadlock Avoidance
   - 6.6 The Dirty Victim Eviction Invariant (Flushing Before Reuse)
7. [Phase 4 Deep-Dive: Benchmarking & Buffered I/O Baseline](#7-phase-4-deep-dive-benchmarking--buffered-io-baseline)
   - 7.1 Why We Need a Performance Baseline
   - 7.2 Why Averages Lie: The Importance of Tail Latency (p50, p90, p99, p99.9)
   - 7.3 High-Resolution Monotonic Timing (`std::chrono::steady_clock`)
   - 7.4 Measuring Throughput & IOPS
   - 7.5 Statistical Variance & Coefficient of Variation (CoV)
   - 7.6 Linux Kernel Telemetry: `iostat`, `pidstat`, and `perf`
8. [Phase 5 Deep-Dive: Direct I/O (`O_DIRECT`) Implementation](#8-phase-5-deep-dive-direct-io-o_direct-implementation)
   - 8.1 What is Direct I/O?
   - 8.2 The Dual-Caching Penalty: Bypassing the Kernel Page Cache
   - 8.3 The Three Strict Hardware Alignment Rules
   - 8.4 Kernel Block Layer Interactions & Why Misalignment Gives `EINVAL`
   - 8.5 Shifting Writeback and Caching Responsibility to User Space
   - 8.6 Measured Comparison: Buffered vs. Direct I/O Latency
9. [Phase 6 Foundations: Write-Ahead Logging (WAL) & Crash Recovery](#9-phase-6-foundations-write-ahead-logging-wal--crash-recovery)
   - 9.1 The Fundamental Challenge: Surviving Sudden Power Loss
   - 9.2 The WAL Durability Invariant
   - 9.3 Structure of a WAL Record
   - 9.4 The Log Sequence Number (LSN) & Page LSN
   - 9.5 The Recovery Process: Analysis, Redo, and Index Rebuild
   - 9.6 Crash Injection Methodologies
10. [Glossary of Systems & Database Terms](#10-glossary-of-systems--database-terms)

---

## 1. Core Foundations: Why Do Databases Manage Storage?

### 1.1 The Hardware Memory Hierarchy & Latency Numbers
To understand why databases are designed the way they are, we must look at the physical limitations of computer hardware:

```
+-------------------------------------------------------+
|  Registers (~0.5 - 1 CPU cycle, ~0.5 ns)              |
+-------------------------------------------------------+
|  L1 / L2 / L3 CPU Caches (~1 - 20 ns)                 |
+-------------------------------------------------------+
|  Main Memory / DRAM (~50 - 100 ns)                    |
+-------------------------------------------------------+
|  Non-Volatile SSD / NVMe (~10 - 100 microseconds)     |  <-- 1,000x slower than RAM
+-------------------------------------------------------+
|  Mechanical HDD (~5 - 10 milliseconds)                |  <-- 100,000x slower than RAM
+-------------------------------------------------------+
```

If we scale 1 CPU cycle (0.5 nanoseconds) to **1 second of human time**:
- **L1 Cache access**: ~2 seconds
- **DRAM access**: ~2.5 minutes
- **NVMe SSD I/O**: ~2 to 3 days!
- **Mechanical HDD seek**: ~3 to 4 months!

Because reading from persistent disk is thousands of times slower than accessing DRAM, a database cannot afford to treat disk as a normal byte-addressable array.

### 1.2 Volatility vs. Durability
- **RAM (DRAM)** is *volatile*: If power cuts out, all bits flip to random electrical noise. Data is lost.
- **Disk (NVMe / SSD / HDD)** is *non-volatile*: Data remains physically encoded in flash memory cells or magnetic platters across power cycles.

The central challenge of database engineering is **giving applications the speed of DRAM while guaranteeing the durability of Disk**.

### 1.3 Why Can't We Just Use Virtual Memory or `mmap`?
Operating systems already have virtual memory and `mmap()` (which maps a file directly into virtual address space). Why don't all databases simply `mmap` their data files?
1. **Flushing Control**: The OS kernel decides when to write dirty pages to disk based on general system memory pressure, not database transaction commit boundaries. This violates Write-Ahead Logging (WAL) rules!
2. **Double Paging**: The OS doesn't know database access patterns (e.g., sequential table scans vs. index lookups) and may evict the root of a B-tree to keep a temporary scan page.
3. **Error Handling**: An I/O error during a page fault in `mmap` raises a `SIGBUS` signal, terminating the process unless delicate signal handlers are written.

Therefore, high-performance database systems (PostgreSQL, MySQL InnoDB, Oracle) manage their own memory pool — called a **Buffer Pool**.

---

## 2. Phase 0 Deep-Dive: The Anatomy of a Database Disk Page

### 2.1 Why 4096 Bytes (4 KiB)?
Why is `PAGE_SIZE = 4096` bytes universally chosen as the fundamental unit of I/O?
1. **Hardware Alignment (Advanced Format 4Kn SSDs)**: Physical SSD flash blocks and modern HDD sectors are structured in 4096-byte physical sectors. An unaligned or partial write forces a "Read-Modify-Write" cycle in hardware, destroying I/O performance and SSD endurance.
2. **OS Virtual Memory Pages**: The x86-64 and ARM64 architecture memory management units (MMUs) map virtual memory to physical frames in 4 KiB granularities.
3. **Filesystem Block Size**: Linux `ext4` and `xfs` allocate disk space in 4 KiB block increments.

Matching the database page size to 4 KiB ensures zero CPU-to-disk impedance mismatch.

---

### 2.2 The Slotted Page Architecture

#### The Slotted Page Solution (Converging Two-Pointer Design)
BufferScope implements the canonical **Slotted Page Layout**:

```
+--------------------------------------------------------------------------+
|  PageHeader (32 bytes)                                                   |
+--------------------------------------------------------------------------+
|  Slot 0: (offset, len) | Slot 1: (offset, len) | Slot 2: ...             |  ---> Grows DOWN
|  (Slot Directory: 4 bytes per record)                                    |
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -+
|                                                                          |
|                            FREE SPACE AREA                               |
|                                                                          |
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -+
|                                                Record 2 [key, val]       |  <--- Grows UP
+--------------------------------------------------------------------------+
|                        Record 1 [key, val]                               |
+--------------------------------------------------------------------------+
|  Record 0 [key, val]                                                     |
+--------------------------------------------------------------------------+
  Byte 0                                                           Byte 4096
```

1. **Header**: Occupies bytes `[0 .. 31]` (`HEADER_SIZE = 32`).
2. **Slot Directory**: Starts at byte 32 and grows **downwards** (towards high memory addresses). Each slot contains:
   ```cpp
   struct SlotEntry {
       uint16_t offset;  // Byte offset where the record begins
       uint16_t length;  // Total byte size of the record
   };
   ```
3. **Record Payloads**: Are inserted at the bottom of the page and grow **upwards** (towards low memory addresses).
   - `free_space_offset` points to the start of the lowest record. Initially `free_space_offset = 4096`.
   - When inserting a record of size $L$, it is placed at byte offset `free_space_offset - L`, and `free_space_offset` is decremented by $L$.
4. **Collision Detection**: The page is full when:
   $$\text{free\_space\_offset} - L < \text{HEADER\_SIZE} + (\text{record\_count} + 1) \times 4$$
5. **O(1) Record Access**: To read slot $i$, we simply inspect byte `32 + i * 4`, retrieve its `offset` and `length`, and immediately read the record bytes without touching any other records!
6. **Deletion (Tombstoning)**: Setting `slot.length = 0` marks the record as deleted. The slot index remains valid, preventing other slot IDs from shifting.

---

### 2.3 The Page Header and Integrity Verification
Every page in BufferScope begins with a strictly defined 32-byte header:

```cpp
struct PageHeader {
    uint32_t magic;             // Magic number (0xBF5C0E01) for file type verification
    uint32_t page_id;           // Logical ID of this page (matches file offset / 4096)
    uint64_t lsn;               // Log Sequence Number / version for WAL
    uint16_t record_count;      // Total number of slot entries
    uint16_t free_space_offset; // Lower boundary of record payload area
    uint32_t checksum;          // CRC32 checksum of page (with this field zeroed)
    uint64_t reserved;          // Padding to ensure exact 32-byte alignment
};
```

---

### 2.4 CRC32: Mathematical Basis & Fast Lookup Table Implementation
For BufferScope, we use **CRC32 (ISO 3309 / ITU-T V.42)** with the reversed polynomial `0xEDB88320`.
By precomputing a 256-element table representing the polynomial division for every possible byte value `0x00 .. 0xFF`, we can process a whole byte in **1 table lookup and XOR operation**:

```cpp
uint32_t crc32(const std::byte* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = static_cast<uint8_t>(data[i]);
        crc = (crc >> 8) ^ crc_table[(crc ^ byte) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFu;
}
```

---

## 3. Operating System File I/O Mechanics

### 3.1 The Linux Page Cache
When a program calls standard POSIX `write()`, data is written to the **OS Page Cache** in kernel RAM. It returns immediately in microseconds, and kernel background threads (`kworker/flush`) write dirty blocks to physical disk later.

### 3.2 POSIX File APIs: `pread` & `pwrite` vs. `lseek`
Unlike `lseek()` + `read()` (which modifies a shared file descriptor offset and is prone to race conditions), `pread()` and `pwrite()` take an explicit byte offset and execute **atomically and thread-safely**.

### 3.3 Durability Barriers: `fsync` vs. `fdatasync`
- **`fsync(fd)`**: Flushes data blocks and inode metadata (updating `mtime`, `atime`).
- **`fdatasync(fd)`**: Flushes data blocks only, skipping non-essential metadata writes. This halves physical write operations in append-heavy database workloads.

---

## 4. Phase 1 Deep-Dive: Building a Minimal Persistent Key-Value Store

### 4.1 From Flat Pages to a Key-Value Store
The `KVStore` layer maps keys (`std::string`) to physical page coordinates: `{page_id, slot_idx}`.
- **Index**: Maintained via `KVIndex` (in-memory hash map).
- **Updates**: Performed via tombstoning old slots and appending new versions into the active page.
- **Recovery**: Reconstructed on startup by scanning pages $0 \dots N-1$, verifying CRC32 checksums, and populating live slots.
- **Verification**: 10,000 randomized operations against an independent `std::unordered_map` reference model.

---

## 5. Phase 2 Deep-Dive: The Database Buffer Pool Manager

### 5.1 What is a Buffer Pool?
A buffer pool is a fixed array of in-memory page frames (e.g., 64 frames = 256 KiB, or 16,384 frames = 64 MiB) maintained by the database engine in user-space DRAM. It intercepts read and write requests:
- **Cache Hit**: Requested `page_id` is resident. Returned immediately with zero disk I/O.
- **Cache Miss**: Page is not in memory. Read from disk into a memory frame.

### 5.2 The Canonical Frame Structure & Memory Alignment (`alignas(4096)`)
```cpp
struct Frame {
    uint32_t page_id;                   // Logical page currently residing in this frame
    uint32_t pin_count;                 // Active concurrent readers/writers
    bool dirty;                         // Modified since loaded?
    bool valid;                         // Holds valid page data?
    bool referenced;                    // Reference bit for CLOCK algorithm
    uint64_t last_access;               // Timestamp counter for LRU
    alignas(4096) std::byte data[4096]; // 4 KiB memory-aligned buffer
};
```
Applying `alignas(4096)` guarantees hardware memory alignment, fulfilling kernel requirements for **Phase 5's Direct I/O (`O_DIRECT`)**.

### 5.3 The Pin / Unpin Protocol
- `FetchPage(page_id)` increments `pin_count++`. Pinned frames **cannot be evicted** while in use.
- `UnpinPage(page_id, is_dirty)` decrements `pin_count--` and marks `dirty = true` if modified. Only frames with `pin_count == 0` become candidates for eviction.

---

## 6. Phase 3 Deep-Dive: CLOCK Replacement & Dirty-Page Flushing

### 6.1 Why Page Replacement is the Core of Storage Performance
When memory fills up and a new page must be brought in from disk, an eviction algorithm decides which page to remove to minimize total disk I/O.

### 6.2 Comparison of Algorithms: Optimal, FIFO, LRU, and CLOCK

| Algorithm | Mechanism | Advantages | Disadvantages |
|---|---|---|---|
| **Optimal (Bélády / MIN)** | Evicts the page needed farthest in the future. | Theoretical maximum hit ratio. | Impossible to implement (requires knowledge of future). |
| **FIFO (First In First Out)** | Evicts the oldest loaded page. | Trivial to implement. | Suffers from **Bélády's Anomaly**; terrible hit ratio. |
| **LRU (Least Recently Used)** | Evicts the page unused for longest time. | Very good hit ratio on temporal locality. | High locking overhead (moves node on every hit). |
| **CLOCK (Second Chance)** | Circular scan with a single reference bit. | **Approximates LRU at near-zero CPU overhead!** | Slightly lower hit ratio than perfect LRU in synthetic tests. |

### 6.3 Why Production Databases Reject Pure LRU
Pure LRU requires maintaining a doubly-linked list of all cached frames.
Every single time any query reads *any* page in the buffer pool, that page's node must be spliced out of the list and moved to the front:
- This requires acquiring an **exclusive mutex on the LRU list on EVERY cache hit**!
- Under 64-core concurrency, CPU cores spend 90% of their cycles spinning on cache line bouncing and lock contention.

**The CLOCK Solution**:
Instead of reordering a linked list on every hit, queries simply set a single boolean bit:
`frame.referenced = true;`
Setting a bit can be done with a single relaxed atomic store with zero lock contention!

### 6.4 The CLOCK (Second Chance) Algorithm Step-by-Step
The buffer frames are arranged in a logical circular array, with a single moving pointer called the **Clock Hand**:

```
                 [ Frame 0 ] (ref=1, pin=0)
               /             \
[ Frame 3 ]                     [ Frame 1 ] (ref=0, pin=0) <-- VICTIM!
(ref=1, pin=1) \             /
                 [ Frame 2 ] (ref=0, pin=1)
                       ^
                   Clock Hand
```

When a free frame is needed:
1. Examine `frame = frames[clock_hand]`.
2. Advance `clock_hand = (clock_hand + 1) % pool_size`.
3. Check `frame.pin_count`:
   - If `pin_count > 0`: **Skip!** The frame is actively in use and cannot be touched.
4. Check `frame.referenced`:
   - If `true`: Clear it (`frame.referenced = false;`) and continue! The page is granted a **"second chance"**.
   - If `false`: **We found our victim!** Return this frame for eviction.

### 6.5 The "All Frames Pinned" Failure Mode & Deadlock Avoidance
What happens if all frames have `pin_count > 0`?
- **System Rule**: If the clock hand completes 2 full sweeps across all frames and every single frame has `pin_count > 0`, `ClockReplacer` immediately returns `std::nullopt`.
- `BufferPoolManager::fetch_page` returns `nullptr`, cleanly signaling that memory is temporarily pinned without deadlocking or crashing.

### 6.6 The Dirty Victim Eviction Invariant (Flushing Before Reuse)
Before a victim frame is overwritten with new page data:
$$\text{if } (\text{victim.dirty}) \implies \text{MUST flush victim to disk FIRST!}$$
Overwriting an unwritten dirty page destroys committed data. In BufferScope, this invariant is enforced unconditionally:
1. Reconstruct page from `victim.data`.
2. Compute new CRC32 checksum.
3. Write page to disk via `FileManager::write_page()`.
4. Clear `victim.dirty = false`.
5. Remove `victim.page_id` from page table.
6. Install new page.

---

## 7. Phase 4 Deep-Dive: Benchmarking & Buffered I/O Baseline

### 7.1 Why We Need a Performance Baseline
Before introducing Direct I/O (`O_DIRECT` in Phase 5), we need a rigorous, repeatable benchmark harness that measures the storage engine under ordinary **Buffered I/O (OS Page Cache)**.
Without a clean baseline, it is impossible to determine whether a custom buffer pool is helping or hurting!

### 7.2 Why Averages Lie: The Importance of Tail Latency
In distributed systems and databases, reporting only the **mean (average) latency** is misleading:
- Suppose 99 queries complete in 10 microseconds (memory hit), and 1 query takes 10 milliseconds (SSD read stall).
- Mean latency: $\frac{99 \times 0.01 + 10}{100} \approx 0.11 \text{ ms}$ (looks fast!).
- However, the 99th percentile customer waited **100x longer**!

Therefore, all BufferScope benchmarks report:
- **p50 (Median)**: The typical experience.
- **p90**: Slower operations.
- **p99**: Tail latency (I/O stalls, lock contention, page writebacks).
- **p99.9**: Extreme outliers.

### 7.3 High-Resolution Monotonic Timing (`std::chrono::steady_clock`)
To measure nanosecond/microsecond latencies accurately:
```cpp
auto start = std::chrono::steady_clock::now();
// ... execute operation ...
auto end = std::chrono::steady_clock::now();
auto duration_us = std::chrono::duration<double, std::micro>(end - start).count();
```
`std::chrono::steady_clock` is monotonic and immune to system clock adjustments (NTP).

### 7.4 Measuring Throughput & IOPS
- **IOPS (Input/Output Operations Per Second)**:
  $$\text{IOPS} = \frac{\text{Total Operations}}{\text{Elapsed Time (seconds)}}$$
- **Bandwidth / Throughput**:
  $$\text{MB/s} = \frac{\text{Total Bytes Transferred}}{1024 \times 1024 \times \text{Elapsed Time (seconds)}}$$

### 7.5 Statistical Variance & Coefficient of Variation (CoV)
To declare a benchmark "stable", we measure the **Coefficient of Variation (CoV)** across multiple independent runs:
$$\text{CoV} = \frac{\sigma}{\mu} \times 100\%$$
Where:
- $\mu$ is the mean throughput across runs.
- $\sigma$ is the sample standard deviation.

**Stability Criterion**:
A benchmark harness is considered stable if $\text{CoV} < 10\%$.
In our Phase 4 measurements:
$$\text{Measured Throughput} = 58,045.0 \pm 944.0 \text{ op/s} \implies \text{CoV} = 1.6\% \ll 10\%$$
This confirms the baseline is exceptionally stable and ready for comparative studies.

---

## 8. Phase 5 Deep-Dive: Direct I/O (`O_DIRECT`) Implementation

### 8.1 What is Direct I/O?
When opening a file with the `O_DIRECT` flag in Linux:
```c
int fd = open(path, O_RDWR | O_DIRECT);
```
The operating system VFS is instructed to **completely bypass the kernel page cache**:
- Data transfers occur directly between user-space memory buffers and the storage controller via **Direct Memory Access (DMA)**.
- Kernel memory is never allocated for caching file blocks.

```
+-------------------------------------------------------------+
|                      Client / KVStore                       |
+-------------------------------------------------------------+
                               |
                        Fetch / Unpin
                               v
+-------------------------------------------------------------+
|                     BufferPoolManager                       |
|   (Direct frame buffer in RAM: alignas(4096) Frame.data)    |
+-------------------------------------------------------------+
               |                               |
       [ Buffered Path ]              [ Direct I/O Path ]
               |                         (open with O_DIRECT)
               v                               |
+-----------------------------+                |
|      Linux Page Cache       |                |  <--- Bypassed!
|   (Kernel memory replica)   |                |       Zero-copy DMA
+-----------------------------+                |
               |                               |
               +---------------+---------------+
                               |
                               v
                     [ NVMe / Block Device ]
```

### 8.2 The Dual-Caching Penalty: Bypassing the Kernel Page Cache
When running a database-managed buffer pool on top of normal buffered I/O:
1. Frame 14 in the database buffer pool holds Page 42 (in user space RAM).
2. The Linux kernel page cache ALSO holds Page 42 (in kernel space RAM).

This **Double Buffering** wastes up to 50% of total system DRAM!
By enabling `O_DIRECT`, all caching is performed exclusively inside the database buffer pool, doubling effective cache capacity.

### 8.3 The Three Strict Hardware Alignment Rules
Direct I/O interacts directly with storage hardware (DMA). Because the CPU cannot perform bounce buffering, the Linux kernel enforces **three strict alignment rules**:
1. **Memory Buffer Alignment**: The pointer passed to `pread()` or `pwrite()` must be an integer multiple of the device logical sector size (typically 4096 or 512 bytes).
2. **File Offset Alignment**: The `offset` parameter must be an integer multiple of the sector size.
3. **Transfer Length Alignment**: The `count` parameter must be an integer multiple of the sector size.

### 8.4 Kernel Block Layer Interactions & Why Misalignment Gives `EINVAL`
If a program calls `pread(fd, unaligned_buf, 4096, 0)` with `O_DIRECT`:
- The Linux kernel does not attempt to fix the pointer; it immediately returns `-1` with `errno = EINVAL` (Invalid Argument)!
- In BufferScope, our canonical `Frame` struct was specifically declared with:
  ```cpp
  alignas(4096) std::byte data[PAGE_SIZE];
  ```
  And all file offsets are `page_id * PAGE_SIZE = page_id * 4096`.
  Thus, our frames natively satisfy all three hardware alignment rules!

### 8.5 Shifting Writeback and Caching Responsibility to User Space
With `O_DIRECT`, the Linux kernel will **never** automatically write dirty pages to disk:
- If our database crashes or fails to call `write_page()`, unwritten data is lost forever.
- Eviction order, cache hit ratios, and dirty writeback are 100% under our direct software control.

### 8.6 Measured Comparison: Buffered vs. Direct I/O Latency
In our Phase 4 and Phase 5 empirical measurements:
- **Median (p50) Latency**:
  - Buffered I/O: `12.46 us`
  - Direct I/O (`O_DIRECT`): `12.77 us`
  *(Essentially identical, because cache hits are serviced directly from our user-space buffer pool!)*
- **Tail (p99) Latency**:
  - Buffered I/O: `78.89 us` *(writes are absorbed into kernel page cache in RAM)*
  - Direct I/O (`O_DIRECT`): `181.48 us` *(writes go directly over PCIe/DMA to NVMe flash blocks!)*

This difference reveals the fundamental truth of systems engineering: **Buffered I/O masks device write latency by hiding it in RAM, whereas O_DIRECT exposes true hardware write latency directly to the application!**

---

## 9. Phase 6 Foundations: Write-Ahead Logging (WAL) & Crash Recovery

### 9.1 The Fundamental Challenge: Surviving Sudden Power Loss
Suppose a transaction updates 5 different pages in the buffer pool.
While page 2 is being written to disk, the server loses power!
- Disk now has: Page 1 (new), Page 2 (partially written/torn), Page 3 (old), Page 4 (old), Page 5 (old).
- The database is now internally inconsistent and corrupted!

How do databases provide **Atomicity and Durability (the A and D in ACID)**?
The answer is **Write-Ahead Logging (WAL)**.

---

### 9.2 The WAL Durability Invariant
The golden rule of database systems:
$$\text{Log records for a state change MUST be flushed to disk BEFORE the dirty page itself is flushed!}$$
Formally:
$$\text{page.LSN} \le \text{flushed\_to\_disk\_LSN}$$

If the system crashes:
- The log contains a complete, sequential history of every committed change.
- The database engine replays the log to reconstruct any page modifications that were lost from RAM!

---

### 9.3 Structure of a WAL Record
Every record in the log file has a fixed, compact binary format:

```cpp
struct LogRecordHeader {
    uint64_t lsn;         // Log Sequence Number (monotonically increasing byte offset)
    uint64_t prev_lsn;    // Link to previous log record by same transaction
    uint32_t tx_id;       // Transaction identifier
    uint8_t  type;        // INSERT, UPDATE, DELETE, COMMIT, ABORT, CHECKPOINT
    uint16_t key_len;
    uint16_t val_len;
    uint32_t checksum;    // CRC32 checksum over the log record
};
```

---

### 9.4 The Log Sequence Number (LSN) & Page LSN
- Every byte position in the log is assigned a unique, monotonically increasing 64-bit integer called the **LSN**.
- Whenever a page is modified in the buffer pool:
  1. A log record is generated with a new LSN (e.g. 500).
  2. The page header's `lsn` field is updated: `page.header.lsn = 500`.
- When the buffer pool wants to evict or flush this page:
  - It checks: Is log record 500 already durable on disk?
  - If not: Call `log_manager.flush(500)` FIRST!
  - Once the log is durable: Write the page to disk.

---

### 9.5 The Recovery Process: Analysis, Redo, and Index Rebuild
When BufferScope starts up after an abnormal shutdown or crash:
1. **Analysis Phase**: Scan the WAL forward from the last checkpoint to identify which transactions committed and which were active.
2. **Redo Phase (Repeating History)**:
   - For every log record with `LSN > page.header.lsn`:
   - Re-apply the operation to the page and update `page.header.lsn`.
3. **Undo Phase**: Roll back any uncommitted changes.
4. **Index Rebuild**: Repopulate the in-memory `KVIndex` from the recovered durable pages.

---

### 9.6 Crash Injection Methodologies
To prove that WAL recovery works:
1. Populate database with committed records.
2. Mid-operation, simulate a crash by killing the process or abruptly truncating uncommitted page writes.
3. Restart `KVStore` and invoke `RecoveryManager`.
4. Assert that **all committed records are fully recovered with valid checksums**.

---

## 10. Glossary of Systems & Database Terms

- **Buffer Pool**: An in-memory cache of fixed-size frames managed by the database engine.
- **Frame**: A physical 4096-byte slot of RAM inside the buffer pool.
- **Page**: A logical 4096-byte block of persistent data on disk.
- **Pin Count**: An atomic counter on a memory frame indicating how many concurrent operations are actively accessing that page.
- **Dirty Bit**: A boolean flag indicating whether the in-memory frame has been modified since it was loaded from disk.
- **Reference Bit**: A flag set on cache hits, cleared by the CLOCK hand to give pages a second chance.
- **Direct I/O (`O_DIRECT`)**: A Linux flag instructing the VFS to transfer data directly between user space buffers and block storage, bypassing the kernel page cache.
- **DMA (Direct Memory Access)**: Hardware mechanism allowing storage controllers to read/write system memory without CPU intervention.
- **Coefficient of Variation (CoV)**: Ratio of standard deviation to mean ($\sigma / \mu \times 100\%$), used to quantify benchmark stability.
- **p50 / p99 Latency**: Percentiles indicating that 50% or 99% of requests completed within this timeframe.
- **IOPS**: Input/Output Operations Per Second.
- **Double Buffering**: Unnecessary duplication of data in both database buffer pool and kernel page cache.
- **WAL (Write-Ahead Logging)**: Durability protocol ensuring log changes reach stable storage before dirty pages.
- **LSN (Log Sequence Number)**: Unique, monotonically increasing integer identifying log entries.

---

## 11. Engineering Review of Phases 0–5: Bugs That Passing Tests Missed

All 41 original tests passed, yet an independent review found real defects. The
lessons generalise to any storage engine.

### 11.1 "Declared but never called" instrumentation
`CacheMetrics::record_bytes_read()` existed, the benchmark printed metrics, and
nobody noticed the byte counters were always zero. **Lesson:** every counter needs a
test that asserts a *non-zero, exact* value after a known workload (e.g. 6 dirty
pages in a 2-frame pool ⇒ exactly 4 evictions and 6 page write-backs). Count at two
independent layers (buffer pool and FileManager syscalls) and cross-check.

### 11.2 Exception safety in the miss path
On a miss the pool picks a victim, removes it from the page table, then reads the
new page. If that read throws (EIO, checksum mismatch), the frame is in neither the
page table nor the free list — it is *leaked*. Repeat N times and the pool is
"all pinned" forever. **Rule:** every step that can fail after a frame is claimed must
return the frame to the free list (`try { load } catch (...) { release; throw; }`).

### 11.3 Sparse-file holes look like corruption
`pwrite` at offset `7*4096` on a 2-page file silently creates a hole; reading page 3
returns 4096 zero bytes — no magic, wrong CRC. A naïve integrity check then reports
"corruption". BufferScope enforces a **no-holes invariant**: every id below
`page_count()` is a real checksummed page (gaps are filled on write-back). This keeps
"page missing" (beyond EOF → `PageNotFoundError`) distinct from "page corrupt"
(`ChecksumMismatchError`), which Phase 6 recovery depends on.

### 11.4 Never bypass the cache you own
The KV layer fell back to `FileManager::write_page` when a fetch failed. If a newer
copy of that page is dirty in the pool, a later eviction overwrites the direct write
— a lost update. Any path that touches a data page must go through the pool.

### 11.5 Integer width bugs in size checks
`uint16_t record_size = 4 + key_len + val_len;` with a 65,536-byte key wraps to a
tiny number and passes the "fits in page" check. Do bounds arithmetic in `size_t`
*before* narrowing.

### 11.6 Where checksums belong
Computing a 4 KiB CRC32 on every PUT and re-verifying in-memory frames on every GET
cost several microseconds per operation. Production engines (PostgreSQL
`data_checksums`, InnoDB) compute the checksum when a page is **written** and verify
it when a page is **read from disk**; memory is trusted. Moving to that model is also
what makes the buffer-pool vs. page-cache comparison measure caching rather than CRC
throughput.

### 11.7 Benchmark hygiene
Reusing one database file across "independent" runs means each run starts with a
bigger file than the last, and warming up a store that is then closed warms nothing
but the OS page cache. Each run now creates a fresh file, warms the *same* store
instance, resets metrics, then records.

### 11.8 Reference models must exercise the hard cases
A reference model for a pool that never evicts only proves the page table works. The
strengthened test runs 72 scripted sequences (6 pool sizes × 4 access shapes × 3
seeds) against an independent CLOCK/LRU simulator, with pinned pages held across
steps, and asserts hits, misses, evictions and residency after every step.

---

## 12. Phase 6 As Built: WAL, Page LSNs, Full-Page Images and Crash Injection

### 12.1 Why "replay the whole log" is wrong
A first draft re-inserted every logged PUT into the pages at every startup. Pages that
were already flushed then held the record twice, and every restart added another copy.
Replaying a DELETE removed only one copy, so the key **came back**. Redo has to be
*idempotent*: replaying a record that is already reflected on the page must do nothing.

### 12.2 Page LSN: the idempotence key
Every page header stores `lsn`, the LSN of the last log record applied to it. Redo rule:
`if (page.lsn < record.lsn) { apply(record); page.lsn = record.lsn; }`. Pages are written
whole and checksummed, so a valid page is either entirely before or entirely after a
given record.

### 12.3 Physiological records
"Insert (k, v)" alone is not deterministic. Where does it go? Each record names
`(page_id, slot)`. On redo, a PUT must find `record_count == slot` on its page; anything
else means the log and the page disagree, and recovery stops loudly.

### 12.4 The two orderings that make WAL work
1. **Commit rule** — a mutation is reported successful only after its COMMIT record is
   fdatasync'ed (`Durability::SYNC`).
2. **WAL-before-data rule** — a page is written only after the log is durable up to
   its LSN (the buffer pool's before-write hook calls `flush_to(page_lsn)`).
If you break (1) you lose acknowledged writes. If you break (2) a page can contain changes
that no surviving log record explains.

### 12.5 Torn pages and full-page images
A 4 KiB write is not atomic on most storage, so a crash can leave half old, half new.
The checksum detects that, but redo cannot fix it, because it needs a correct base page.
PostgreSQL's answer (`full_page_writes`) is used here: the first time a page changes after
a checkpoint, its whole before-image goes into the WAL. Recovery restores torn or missing
pages from that image and redoes the later records. The cost is one 4 KiB log record per
page per checkpoint interval.

### 12.6 Checkpoints
Checkpoint = flush WAL → write all dirty pages → fdatasync data file → atomically replace
the WAL with a single CHECKPOINT record (write temp, fdatasync, `rename`, fsync the
directory). If the process crashes between the page fdatasync and the rename, recovery
sees log records that are already on the pages and skips them by page LSN. That is crash
point (c), and its test asserts `redo_applied == 0`.

### 12.7 Distinct failure modes
| Situation | Detection | Outcome |
|---|---|---|
| Torn/corrupt page **inside** the crash window | CRC mismatch, image in WAL | repaired from image |
| Corrupt page **outside** the crash window | CRC mismatch, no image | `ChecksumMismatchError` |
| Page missing but recorded at checkpoint | `page_count` < checkpoint count | `PageNotFoundError` |
| Torn WAL tail | short record / bad CRC / LSN gap | truncated; incomplete op discarded |

### 12.8 Crash injection that proves "no more, no less"
Tests fork a child, arm one named crash point (`WAL_AFTER_FLUSH`, `PAGE_BEFORE_WRITE`,
`PAGE_TORN_WRITE`, `CKPT_AFTER_PAGE_FLUSH`, …), and the child reports every acknowledged
op over a pipe. When the point fires the child is killed with SIGKILL, so no destructors
or flushes run. The parent recovers and compares against an oracle replay of exactly the
durable prefix. One subtle case: at `WAL_AFTER_FLUSH` the op is **committed but not
acknowledged**, so it must survive (acked + 1). Power loss is simulated separately by
truncating the WAL at every record boundary and at points inside records.

### 12.9 Durability vs. throughput
On the secondary test machine, fdatasync per mutation costs ~130 µs, which caps a
single thread at ~7.7 k mutations/s versus ~1.2 M op/s without a WAL. Group commit
amortises the sync over N mutations at the cost of losing up to N−1 acknowledged ops in a
crash. That is a trade-off to document, not a bug to hide.

---

## 13. Phase 7: Reproducible Workloads

### 13.1 Why not `std::uniform_int_distribution`?
The C++ standard specifies the *engines* (e.g. `mt19937_64` produces the same numbers
everywhere) but not the *distributions*. libstdc++, libc++ and MSVC turn the same engine
output into different integers. An experiment that must be "exactly reproducible" therefore
needs its own distribution code. BufferScope uses SplitMix64 plus Lemire's unbiased
multiply-shift for bounded integers, so `uniform`, `sequential` and `hotspot` are
bit-identical on every platform. Golden hashes in the tests catch regressions.

### 13.2 The access patterns and what they test
| Pattern | Definition | Buffer-manager question it answers |
|---|---|---|
| sequential | key = seq mod N | scan resistance: does a one-pass scan flush the hot set? |
| uniform | every key equally likely | no locality: hit ratio ≈ pool / data size |
| hotspot h/o | o% of ops hit the first h% of keys | does the policy keep the hot set resident? (80/20, 95/5) |
| zipf θ | P(rank r) ∝ 1/r^θ (YCSB generator) | heavy-tailed real-world skew |

Because keys are loaded in key order, key id ↔ page is monotone. The hot set of a hotspot
workload is therefore a contiguous run of pages: 20% of keys means 20% of pages.
`--scramble` hashes key ids to spread hot keys over all pages, which models the much
weaker page-level locality you get when hot rows are scattered.

### 13.3 Cache state is part of the experiment
`warm`: the pool and the OS cache are both warmed by a warm-up phase on the same store.
`oswarm`: the pool is emptied but the OS page cache still holds the file. Every pool miss
then costs a syscall + memcpy, not a device read. This is the "double caching" regime.
`cold`: pool emptied *and* the file dropped from the OS cache with `posix_fadvise(DONTNEED)`
(no root needed). The runner records `os_cache_resident_before` (via `mincore`) so the state
is *measured*, not assumed.

### 13.4 Out-of-place vs in-place updates
Appending every new version (log-structured style) is simple, but under a write-heavy mix
it grows the file without bound and constantly moves hot keys to new pages. That defeats
the buffer pool. An in-place update when the record fits (fixed-size values always fit)
keeps the working set stable. It is logged as a physiological `UPDATE(page, slot, k, v)`
redo record, which is idempotent under the page-LSN rule exactly like PUT.
