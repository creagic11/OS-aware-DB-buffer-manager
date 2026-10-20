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

## Notes

- **WSL2 environment**: Running on Windows Subsystem for Linux 2. The virtual
  disk is backed by a VHDX file on the host's NVMe SSD. I/O latency and
  throughput may differ from a bare-metal Linux installation.
- **O_DIRECT support**: Will be verified in Phase 5. ext4 on WSL2 generally
  supports O_DIRECT, but this needs explicit testing.
- **perf**: Available at `/usr/local/bin/perf`. May have limited functionality
  under WSL2 due to the virtualized kernel.
- **iostat/pidstat**: Available via sysstat package. Will be used for
  storage-device-level metrics in Phase 8.
