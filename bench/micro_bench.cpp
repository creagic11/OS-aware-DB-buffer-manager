/**
 * @file micro_bench.cpp
 * @brief Google Benchmark microbenchmarks for the hot paths (Phase 4).
 *
 * These isolate per-component CPU cost (CRC32, slotted-page insert, buffer-pool
 * hit path, KV get on a resident page) so end-to-end numbers from bench_runner
 * can be attributed to software vs. storage.
 */

#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/kv_store.hpp"

#include <benchmark/benchmark.h>
#include <filesystem>
#include <unistd.h>

using namespace bufferscope;

namespace {
std::string tmp_path(const char* tag) {
    return "/tmp/bufferscope_micro_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".db";
}
}  // namespace

static void BM_CRC32_Page(benchmark::State& state) {
    Page p(0u);
    for (auto _ : state) benchmark::DoNotOptimize(compute_page_checksum(p.data()));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * PAGE_SIZE);
}
BENCHMARK(BM_CRC32_Page);

static void BM_PageInsert(benchmark::State& state) {
    const std::string key = "key_000123", val(64, 'v');
    for (auto _ : state) {
        Page p(0u);
        while (p.insert_record(key, val)) {
        }
        benchmark::DoNotOptimize(p.record_count());
    }
}
BENCHMARK(BM_PageInsert);

static void BM_BufferPoolHit(benchmark::State& state) {
    auto path = tmp_path("bpmhit");
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
    {
        FileManager fm(path);
        BufferPoolManager bpm(64, fm);
        for (uint32_t p = 0; p < 64; ++p) {
            bpm.fetch_page(p);
            bpm.unpin_page(p, false);
        }
        uint32_t p = 0;
        for (auto _ : state) {
            benchmark::DoNotOptimize(bpm.fetch_page(p));
            bpm.unpin_page(p, false);
            p = (p + 1) & 63;
        }
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
}
BENCHMARK(BM_BufferPoolHit);

static void BM_KVGetResident(benchmark::State& state) {
    auto path = tmp_path("kvget");
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
    {
        KVStore kv(path, 256);
        for (int i = 0; i < 1000; ++i) kv.put("key_" + std::to_string(i), std::string(64, 'v'));
        int i = 0;
        for (auto _ : state) {
            benchmark::DoNotOptimize(kv.get("key_" + std::to_string(i)));
            i = (i + 1) % 1000;
        }
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
}
BENCHMARK(BM_KVGetResident);

BENCHMARK_MAIN();
