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
