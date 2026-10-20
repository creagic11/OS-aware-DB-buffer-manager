# BufferScope — Environment Metadata

## Machine Configuration

| Property           | Value                                          |
|--------------------|-------------------------------------------------|
| **CPU**            | 13th Gen Intel Core i5-1340P (8 cores / 16 threads) |
| **RAM**            | 7.6 GiB (WSL2 allocated)                       |
| **Storage**        | 1 TB virtual disk (ext4, /dev/sdc)              |
| **OS**             | Ubuntu 24.04.2 LTS (WSL2)                      |
| **Kernel**         | 5.15.167.4-microsoft-standard-WSL2              |
| **Filesystem**     | ext4 (rw,relatime,discard,errors=remount-ro,data=ordered) |
| **Compiler**       | GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)    |
| **CMake**          | 3.28.3                                          |
| **Ninja**          | 1.11.1                                          |
| **GoogleTest**     | 1.14.0                                          |
| **Google Benchmark** | 1.8.3                                         |
| **Python**         | 3.12.3 (with pandas 2.1.4, matplotlib 3.6.3)   |

## Benchmark Data Directory

| Property        | Value                                            |
|-----------------|--------------------------------------------------|
| **Path**        | ~/bufferscope/data/                              |
| **Filesystem**  | ext4                                             |
| **Mount**       | /dev/sdc on / (rw,relatime,discard)              |
| **Free Space**  | ~951 GB                                          |

## Direct I/O (O_DIRECT) Alignment Requirements

| Property                     | Measured Value                                   |
|------------------------------|--------------------------------------------------|
| **Logical Sector Size**      | 512 bytes (`/sys/block/sdc/queue/logical_block_size`) |
| **Physical Sector Size**     | 4096 bytes (`/sys/block/sdc/queue/physical_block_size`) |
| **Enforced Frame Alignment** | 4096 bytes (`alignas(4096) std::byte data[4096]`) |
| **O_DIRECT Support Status**  | Verified functional (`open` with `O_DIRECT` confirmed on `/dev/sdc`) |
| **Misalignment Behavior**    | Kernel returns `-1` (`errno == EINVAL`)           |

## Notes

- **WSL2 environment**: Running on Windows Subsystem for Linux 2. The virtual disk is backed by a VHDX file on the host's NVMe SSD.
- **O_DIRECT support**: Verified on ext4. Alignment to 4096 bytes satisfies both logical (512 B) and physical (4096 B) sector sizes.
- **iostat/pidstat**: Confirmed working (`sysstat` package) for storage-level telemetry.

---

## Secondary Verification Environment (review of Phases 0–5, continued development)

The Phase 0–5 review and Phases 6–9 were built and verified in a second, clean
Linux environment (a fresh checkout built from scratch). Numbers measured here are
labelled with this environment; numbers measured on the WSL2 machine above stay
labelled with that machine. They are **not** interchangeable.

| Property           | Value |
|--------------------|-------|
| **CPU**            | Intel Xeon @ 2.10 GHz, 2 vCPUs (cloud VM) |
| **RAM**            | 7 GiB |
| **OS / Kernel**    | Ubuntu 24.04.5 LTS, Linux 6.18 (VM) |
| **Filesystem**     | ext4 on `/dev/vda` (`rw,relatime,discard`) — block-device backed, not tmpfs |
| **Logical / physical sector** | 512 B / 4096 B (`/sys/block/vda/queue/*_block_size`) |
| **O_DIRECT**       | Supported; 4096-byte alignment (= max(sector, PAGE_SIZE)) verified by `DirectIOTest` and `io_mode_correctness_test` |
| **Toolchain**      | GCC 13.3.0, CMake 3.28.3, Ninja, GoogleTest 1.14.0, Google Benchmark 1.8.3, Python 3.12 + pandas 3.0.5 + matplotlib 3.11.2 |

### Phase 8 measurement tooling (secondary environment)

| Item | Value |
|---|---|
| Device baseline (runner, QD1) | ~15–18 k random 4 KiB reads/s, `r_await` ≈ 0.04 ms, virtio `vda` |
| iostat / pidstat | sysstat 12.x (`iostat -x -y -t 1` logged per experiment) |
| perf | `/usr/lib/linux-tools/6.8.0-147-generic/perf`; software events only (hardware counters not exposed in the VM) |
| Memory cgroups | cgroup v1 `memory` controller; page cache verified to be charged and capped (300 MiB file in a 96 MiB cgroup → 95 MiB resident) |
| Cold cache | `posix_fadvise(DONTNEED)` on the data file (no root needed); `drop_caches` available but not required |
| Snapshot | `results/raw/environment.txt` (written by every matrix run) |
