/**
 * @file runner.cpp
 * @brief Benchmark runner supporting both BUFFERED and DIRECT I/O modes (Phase 4 & 5).
 */

#include "bufferscope/kv_store.hpp"
#include "bufferscope/metrics.hpp"
#include "workload.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <vector>
#include <numeric>
#include <cmath>

using namespace bufferscope;

struct RunResult {
    double elapsed_sec = 0.0;
    double iops = 0.0;
    double hit_ratio = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double mean_us = 0.0;
    double stddev_us = 0.0;
};

// Each run is independent: a fresh database file, a warm-up pass on the SAME
// store instance (so the measured pass starts from a warm buffer pool), then
// metrics are reset and only the steady-state pass is recorded.
RunResult execute_workload_run(const std::string& db_path,
                               const std::vector<Operation>& warmup_ops,
                               const std::vector<Operation>& ops,
                               size_t pool_size,
                               IOMode mode) {
    std::filesystem::remove(db_path);
    KVStore store(db_path, pool_size, mode);
    auto apply = [&store](const Operation& op) {
        if (op.type == OpType::PUT) store.put(op.key, op.value);
        else if (op.type == OpType::GET) (void)store.get(op.key);
        else store.remove(op.key);
    };
    for (const auto& op : warmup_ops) apply(op);
    store.buffer_pool().mutable_metrics().reset();

    LatencyTracker latency;
    latency.reserve(ops.size());
    auto start_time = std::chrono::steady_clock::now();
    for (const auto& op : ops) {
        auto t0 = std::chrono::steady_clock::now();
        apply(op);
        auto t1 = std::chrono::steady_clock::now();
        latency.record_micros(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    store.flush();
    auto end_time = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    RunResult res;
    res.elapsed_sec = elapsed;
    res.iops = static_cast<double>(ops.size()) / elapsed;
    res.hit_ratio = store.buffer_pool().metrics().hit_ratio();
    res.p50_us = latency.p50();
    res.p90_us = latency.p90();
    res.p99_us = latency.p99();
    res.mean_us = latency.mean();
    res.stddev_us = latency.stddev();
    std::filesystem::remove(db_path);
    return res;
}

int main(int argc, char* argv[]) {
    size_t num_ops = 50000;
    size_t key_space = 1000;
    size_t num_runs = 5;
    size_t pool_size = 128;
    std::string db_dir = "/tmp";
    IOMode mode = IOMode::BUFFERED;
    std::string mode_str = "buffered";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ops" && i + 1 < argc) num_ops = std::stoul(argv[++i]);
        else if (arg == "--keys" && i + 1 < argc) key_space = std::stoul(argv[++i]);
        else if (arg == "--runs" && i + 1 < argc) num_runs = std::stoul(argv[++i]);
        else if (arg == "--pool" && i + 1 < argc) pool_size = std::stoul(argv[++i]);
        else if (arg == "--dir" && i + 1 < argc) db_dir = argv[++i];
        else if (arg == "--io-mode" && i + 1 < argc) {
            mode_str = argv[++i];
            mode = parse_io_mode(mode_str);
        } else if (arg.rfind("--io-mode=", 0) == 0) {
            mode_str = arg.substr(10);
            mode = parse_io_mode(mode_str);
        }
    }

    std::cout << "========================================================\n";
    std::cout << "  BufferScope Benchmark Harness (I/O Mode: "
              << (mode == IOMode::DIRECT ? "O_DIRECT" : "BUFFERED") << ")\n";
    std::cout << "========================================================\n";
    std::cout << "Configuration:\n";
    std::cout << "  I/O Mode:           " << (mode == IOMode::DIRECT ? "DIRECT (O_DIRECT)" : "BUFFERED (OS Page Cache)") << "\n";
    std::cout << "  Operations per run: " << num_ops << "\n";
    std::cout << "  Key space size:     " << key_space << " keys\n";
    std::cout << "  Buffer pool size:   " << pool_size << " frames (" << (pool_size * 4) << " KiB)\n";
    std::cout << "  Number of runs:     " << num_runs << "\n";
    std::cout << "  Database directory: " << db_dir << "\n";
    std::cout << "--------------------------------------------------------\n\n";

    WorkloadConfig config;
    config.num_operations = num_ops;
    config.key_space_size = key_space;
    config.value_size = 64;
    config.put_ratio = 0.50;
    config.get_ratio = 0.40;
    config.delete_ratio = 0.10;
    config.pattern = AccessPattern::UNIFORM_RANDOM;
    config.seed = 1337;

    WorkloadGenerator generator(config);
    std::cout << "[1/3] Generating workload operations..." << std::flush;
    auto ops = generator.generate();
    std::cout << " Done (" << ops.size() << " operations ready).\n";

    std::string bench_file = db_dir + "/bufferscope_bench.db";
    std::filesystem::remove(bench_file);

    // Warm-up ops: a different seed, same distribution; applied inside each run.
    WorkloadConfig warmup_cfg = config;
    warmup_cfg.num_operations = 5000;
    warmup_cfg.seed = config.seed + 1;
    auto warmup_ops = WorkloadGenerator(warmup_cfg).generate();
    std::cout << "[2/3] Warm-up: 5,000 ops (seed " << warmup_cfg.seed
              << ") executed at the start of every run before recording.\n\n";

    // Measured Steady-State Benchmark Runs
    std::cout << "[3/3] Executing " << num_runs << " benchmark runs...\n\n";
    std::cout << std::setw(6) << "Run #"
              << std::setw(12) << "Time (s)"
              << std::setw(14) << "Throughput"
              << std::setw(12) << "Hit Ratio"
              << std::setw(12) << "p50 (us)"
              << std::setw(12) << "p90 (us)"
              << std::setw(12) << "p99 (us)"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    std::vector<RunResult> results;
    std::vector<double> iops_list;
    std::vector<double> p50_list;
    std::vector<double> p99_list;

    for (size_t r = 1; r <= num_runs; ++r) {
        RunResult res = execute_workload_run(bench_file, warmup_ops, ops, pool_size, mode);
        results.push_back(res);
        iops_list.push_back(res.iops);
        p50_list.push_back(res.p50_us);
        p99_list.push_back(res.p99_us);

        std::cout << std::setw(6) << r
                  << std::setw(12) << std::fixed << std::setprecision(4) << res.elapsed_sec
                  << std::setw(11) << std::fixed << std::setprecision(0) << res.iops << " op/s"
                  << std::setw(11) << std::fixed << std::setprecision(2) << (res.hit_ratio * 100.0) << "%"
                  << std::setw(12) << std::fixed << std::setprecision(2) << res.p50_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << res.p90_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << res.p99_us
                  << "\n";
    }

    std::cout << std::string(80, '-') << "\n\n";

    auto calc_mean_std = [](const std::vector<double>& v) {
        double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        double acc = 0.0;
        for (double d : v) acc += (d - m) * (d - m);
        double s = std::sqrt(acc / (v.size() > 1 ? v.size() - 1 : 1));
        return std::make_pair(m, s);
    };

    auto [iops_mean, iops_std] = calc_mean_std(iops_list);
    auto [p50_mean, p50_std] = calc_mean_std(p50_list);
    auto [p99_mean, p99_std] = calc_mean_std(p99_list);

    double iops_cov = (iops_std / iops_mean) * 100.0;

    std::cout << "========================================================\n";
    std::cout << "  Summary Statistics (" << (mode == IOMode::DIRECT ? "O_DIRECT" : "BUFFERED") << ")\n";
    std::cout << "========================================================\n";
    std::cout << "Throughput (IOPS): " << std::fixed << std::setprecision(1)
              << iops_mean << " ± " << iops_std << " op/s  (CoV: " << iops_cov << "%)\n";
    std::cout << "p50 Latency:       " << std::fixed << std::setprecision(2)
              << p50_mean << " ± " << p50_std << " us\n";
    std::cout << "p99 Latency:       " << std::fixed << std::setprecision(2)
              << p99_mean << " ± " << p99_std << " us\n";
    std::cout << "========================================================\n";

    std::filesystem::remove(bench_file);
    return 0;
}
