# BufferScope

**OS-Aware Database Buffer Manager**

> Under what workload and memory conditions does a database-managed buffer pool
> outperform ordinary buffered I/O, and what costs does O_DIRECT introduce by
> bypassing the OS page cache?

## Status

🚧 **Phase 0: Disk Page Library** — In Progress

See [docs/PROGRESS.md](docs/PROGRESS.md) for detailed progress tracking.

## Quick Start

```bash
# Build
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja

# Run tests
ctest --output-on-failure

# Run smoke test
./bufferscope
```

## Architecture

```
Client API  →  KVStore/DB API  →  Page Manager
    →  [Buffered path: read/write() → Linux page cache]
    OR [Direct-I/O path: O_DIRECT → Buffer Pool]
    →  Files  →  SSD
```

## Project Structure

```
bufferscope/
├── include/bufferscope/    # Public headers
│   ├── page.hpp            # 4 KiB page abstraction
│   └── file_manager.hpp    # Page-oriented file I/O
├── src/                    # Implementation
│   ├── page.cpp
│   ├── file_manager.cpp
│   └── main.cpp
├── tests/                  # GoogleTest test suites
├── bench/                  # Benchmarks (Phase 4+)
├── docs/                   # Documentation
│   ├── ENVIRONMENT.md
│   └── PROGRESS.md
├── scripts/                # Build & benchmark scripts
├── data/                   # Benchmark data (gitignored)
├── CMakeLists.txt
└── README.md
```
