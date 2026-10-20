/**
 * @file runner.cpp
 * @brief BufferScope benchmark runner (Phases 4, 5, 7).
 *
 * One invocation = one configuration, repeated --runs times. Each run:
 *   1. fresh database file (+ WAL)
 *   2. LOAD: insert every key once in key order (not measured), checkpoint
 *   3. prepare cache state:
 *        warm   : run --warmup-ops ops on the same store (pool + OS cache warm)
 *        oswarm : evict the buffer pool, keep the OS page cache (dual-cache case)
 *        cold   : evict the buffer pool AND drop the file from the OS page cache
 *   4. reset every counter, measure the OS page-cache residency of the file (mincore)
 *   5. RUN: execute the operation sequence, timing every op with steady_clock
 *   6. final checkpoint (timed separately as end_flush_ms)
 *
 * All parameters are runtime flags; nothing experiment-specific is hard-coded.
 * Output: human summary on stdout, --summary CSV (one row per run, appended),
 * --json (config + runs + aggregates), --trace per-op CSV (seq,op,key,latency_ns).
 * Column meanings are documented in scripts/SCHEMA.md.
 */

#include "bufferscope/kv_store.hpp"
#include "bufferscope/metrics.hpp"
#include "workload.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bufferscope;
namespace fs = std::filesystem;

namespace {

enum class CacheState { WARM, OSWARM, COLD };
const char* cache_name(CacheState c) {
    return c == CacheState::WARM ? "warm" : c == CacheState::OSWARM ? "oswarm" : "cold";
}
CacheState parse_cache(const std::string& s) {
    if (s == "warm") return CacheState::WARM;
    if (s == "oswarm") return CacheState::OSWARM;
    if (s == "cold") return CacheState::COLD;
    throw std::invalid_argument("unknown --cache '" + s + "' (warm|oswarm|cold)");
}

struct Options {
    WorkloadConfig wl;
    uint64_t warmup_ops = 0;  // 0 => num_operations / 10
    size_t pool_frames = 128;
    IOMode io_mode = IOMode::BUFFERED;
    Durability durability = Durability::OFF;
    uint32_t group_commit = 64;
    ReplacerType replacer = ReplacerType::CLOCK;
    CacheState cache = CacheState::WARM;
    size_t runs = 5;
    std::string dir = "data";
    std::string label = "run";
    std::string summary_csv, json_path, trace_path, dump_ops;
    bool quiet = false;
};

// ── OS-level counters ─────────────────────────────────────────────────
struct ProcIO {
    uint64_t rchar = 0, wchar = 0, syscr = 0, syscw = 0, read_bytes = 0, write_bytes = 0;
};
ProcIO read_proc_io() {
    ProcIO p;
    std::ifstream f("/proc/self/io");
    std::string k;
    uint64_t v;
    while (f >> k >> v) {
        if (k == "rchar:") p.rchar = v;
        else if (k == "wchar:") p.wchar = v;
        else if (k == "syscr:") p.syscr = v;
        else if (k == "syscw:") p.syscw = v;
        else if (k == "read_bytes:") p.read_bytes = v;
        else if (k == "write_bytes:") p.write_bytes = v;
    }
    return p;
}

struct Usage {
    double utime = 0, stime = 0;
    long minflt = 0, majflt = 0, nvcsw = 0, nivcsw = 0;
};
Usage read_usage() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    Usage u;
    u.utime = static_cast<double>(r.ru_utime.tv_sec) + r.ru_utime.tv_usec / 1e6;
    u.stime = static_cast<double>(r.ru_stime.tv_sec) + r.ru_stime.tv_usec / 1e6;
    u.minflt = r.ru_minflt;
    u.majflt = r.ru_majflt;
    u.nvcsw = r.ru_nvcsw;
    u.nivcsw = r.ru_nivcsw;
    return u;
}

/// Fraction of the file's pages resident in the OS page cache (mincore). -1 on error.
double os_cache_residency(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (::fstat(fd, &st) < 0 || st.st_size == 0) {
        ::close(fd);
        return st.st_size == 0 ? 0.0 : -1;
    }
    void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return -1;
    const long psz = ::sysconf(_SC_PAGESIZE);
    const size_t pages = (static_cast<size_t>(st.st_size) + psz - 1) / psz;
    std::vector<unsigned char> vec(pages);
    double frac = -1;
    if (::mincore(m, static_cast<size_t>(st.st_size), vec.data()) == 0) {
        size_t res = 0;
        for (unsigned char c : vec) res += c & 1;
        frac = static_cast<double>(res) / static_cast<double>(pages);
    }
    ::munmap(m, static_cast<size_t>(st.st_size));
    return frac;
}

// ── One run ───────────────────────────────────────────────────────────
using Row = std::vector<std::pair<std::string, std::string>>;

template <typename T>
std::string str(T v) {
    std::ostringstream o;
    o << std::setprecision(10) << v;
    return o.str();
}

struct RunOutput {
    Row row;                       // all columns for the summary CSV
    std::map<std::string, double> num;  // numeric columns for aggregation
};

RunOutput execute_run(const Options& opt, size_t run_idx, const WorkloadGenerator& gen,
                      const std::vector<Operation>& warm_ops, const std::vector<Operation>& ops) {
    const std::string db = (fs::path(opt.dir) / ("bench_" + std::to_string(::getpid()) + ".db")).string();
    fs::remove(db);
    fs::remove(KVStore::wal_path_for(db));

    KVOptions ko;
    ko.pool_size = opt.pool_frames;
    ko.io_mode = opt.io_mode;
    ko.durability = opt.durability;
    ko.group_commit_ops = opt.group_commit;
    ko.replacer = opt.replacer;

    RunOutput out;
    {
        KVStore store(db, ko);
        const auto& wl = gen.config();

        // LOAD (not measured): every key once, in key order. Durability is irrelevant
        // for the load, so it runs with the store's mode but is not timed.
        std::string val;
        for (uint32_t k = 0; k < wl.key_space_size; ++k) {
            gen.fill_value(UINT64_MAX - k, k, val);
            store.put(WorkloadGenerator::key_for(k), val);
        }
        store.checkpoint();

        auto apply = [&](const Operation& op, uint64_t seq, std::string& v) {
            const std::string key = WorkloadGenerator::key_for(op.key_id);
            switch (op.type) {
                case OpType::GET: (void)store.get(key); break;
                case OpType::PUT: gen.fill_value(seq, op.key_id, v); store.put(key, v); break;
                case OpType::DELETE: store.remove(key); break;
            }
        };

        if (opt.cache == CacheState::WARM) {
            for (uint64_t i = 0; i < warm_ops.size(); ++i) apply(warm_ops[i], (1ull << 62) + i, val);
            store.checkpoint();
        } else {
            store.buffer_pool().evict_all();
            if (opt.cache == CacheState::COLD) store.file_manager().drop_os_cache();
        }

        // Reset all counters; snapshot OS state.
        store.buffer_pool().mutable_metrics().reset();
        store.file_manager().stats().reset();
        if (store.wal()) store.wal()->stats().reset();
        const double residency_before = os_cache_residency(db);
        const uint64_t file_pages = store.file_manager().page_count();
        const ProcIO io0 = read_proc_io();
        const Usage u0 = read_usage();

        // RUN (measured).
        std::vector<uint32_t> lat_ns(ops.size());
        LatencyTracker lat;
        lat.reserve(ops.size());
        std::string v;
        v.reserve(wl.value_size);
        const auto t_start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < ops.size(); ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            apply(ops[i], i, v);
            const auto t1 = std::chrono::steady_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            lat_ns[i] = static_cast<uint32_t>(std::min<int64_t>(ns, UINT32_MAX));
        }
        const auto t_end = std::chrono::steady_clock::now();
        const Usage u1 = read_usage();
        const ProcIO io1 = read_proc_io();

        auto& m = store.buffer_pool().metrics();
        auto& fst = store.file_manager().stats();
        const uint64_t hits = m.get_hits(), misses = m.get_misses(), evictions = m.get_evictions(),
                       dirty_ev = m.get_dirty_evictions(), flushes = m.get_page_flushes(),
                       br = m.get_bytes_read(), bw = m.get_bytes_written(), preads = fst.page_reads,
                       pwrites = fst.page_writes, fsyncs = fst.syncs;
        const uint64_t wal_syncs = store.wal() ? store.wal()->stats().flushes.load() : 0;
        const uint64_t wal_bytes = store.wal() ? store.wal()->stats().bytes.load() : 0;
        const double wal_sync_ms = store.wal() ? store.wal()->stats().flush_ns.load() / 1e6 : 0.0;

        const auto f0 = std::chrono::steady_clock::now();
        store.checkpoint();
        const auto f1 = std::chrono::steady_clock::now();

        for (uint32_t ns : lat_ns) lat.record_micros(ns / 1000.0);
        const double elapsed = std::chrono::duration<double>(t_end - t_start).count();
        size_t n_get = 0, n_put = 0, n_del = 0;
        for (const auto& op : ops) (op.type == OpType::GET ? n_get : op.type == OpType::PUT ? n_put : n_del)++;

        auto add = [&](const std::string& k, double v, bool numeric = true) {
            out.row.emplace_back(k, str(v));
            if (numeric) out.num[k] = v;
        };
        auto add_s = [&](const std::string& k, const std::string& v) { out.row.emplace_back(k, v); };

        add_s("label", opt.label);
        add("run", static_cast<double>(run_idx), false);
        add_s("pattern", pattern_name(wl.pattern));
        add("read_pct", wl.read_pct, false);
        add("delete_pct", wl.delete_pct, false);
        add("keys", wl.key_space_size, false);
        add("ops", static_cast<double>(ops.size()), false);
        add("value_size", wl.value_size, false);
        add("zipf_theta", wl.pattern == AccessPattern::ZIPFIAN ? wl.zipf_theta : 0, false);
        add("hot_keys", wl.pattern == AccessPattern::HOTSPOT ? wl.hot_key_fraction : 0, false);
        add("hot_ops", wl.pattern == AccessPattern::HOTSPOT ? wl.hot_op_fraction : 0, false);
        add("scramble", wl.scramble ? 1 : 0, false);
        add("seed", static_cast<double>(wl.seed), false);
        add("pool_frames", static_cast<double>(opt.pool_frames), false);
        add("pool_mib", opt.pool_frames * 4096.0 / (1 << 20), false);
        add("file_pages", static_cast<double>(file_pages), false);
        add("data_mib", file_pages * 4096.0 / (1 << 20), false);
        add_s("io_mode", io_mode_name(opt.io_mode));
        add_s("durability", durability_name(opt.durability));
        add_s("replacer", replacer_name(opt.replacer));
        add_s("cache_state", cache_name(opt.cache));
        add("os_cache_resident_before", residency_before);
        add("elapsed_s", elapsed);
        add("throughput_ops", ops.size() / elapsed);
        add("p50_us", lat.p50());
        add("p90_us", lat.p90());
        add("p99_us", lat.p99());
        add("p999_us", lat.p99_9());
        add("mean_us", lat.mean());
        add("max_us", lat.max());
        add("gets", static_cast<double>(n_get), false);
        add("puts", static_cast<double>(n_put), false);
        add("deletes", static_cast<double>(n_del), false);
        add("hit_ratio", m.hit_ratio());
        add("hits", static_cast<double>(hits));
        add("misses", static_cast<double>(misses));
        add("evictions", static_cast<double>(evictions));
        add("dirty_evictions", static_cast<double>(dirty_ev));
        add("page_flushes", static_cast<double>(flushes));
        add("bytes_read", static_cast<double>(br));
        add("bytes_written", static_cast<double>(bw));
        add("page_reads", static_cast<double>(preads));
        add("page_writes", static_cast<double>(pwrites));
        add("io_iops", (preads + pwrites) / elapsed);  // page I/Os issued per second
        add("data_fsyncs", static_cast<double>(fsyncs));
        add("wal_syncs", static_cast<double>(wal_syncs));
        add("wal_bytes", static_cast<double>(wal_bytes));
        add("wal_sync_ms", wal_sync_ms);
        add("end_flush_ms", std::chrono::duration<double, std::milli>(f1 - f0).count());
        add("cpu_user_s", u1.utime - u0.utime);
        add("cpu_sys_s", u1.stime - u0.stime);
        add("cpu_util", (u1.utime - u0.utime + u1.stime - u0.stime) / elapsed);
        add("minor_faults", static_cast<double>(u1.minflt - u0.minflt));
        add("major_faults", static_cast<double>(u1.majflt - u0.majflt));
        add("ctx_voluntary", static_cast<double>(u1.nvcsw - u0.nvcsw));
        add("ctx_involuntary", static_cast<double>(u1.nivcsw - u0.nivcsw));
        add("os_read_bytes", static_cast<double>(io1.read_bytes - io0.read_bytes));
        add("os_write_bytes", static_cast<double>(io1.write_bytes - io0.write_bytes));
        add("syscalls_read", static_cast<double>(io1.syscr - io0.syscr));
        add("syscalls_write", static_cast<double>(io1.syscw - io0.syscw));

        if (!opt.trace_path.empty()) {
            std::string tp = opt.trace_path;
            if (opt.runs > 1) {
                auto pos = tp.rfind(".csv");
                tp = (pos == std::string::npos ? tp : tp.substr(0, pos)) + "_run" + std::to_string(run_idx) + ".csv";
            }
            std::ofstream tf(tp, std::ios::trunc);
            tf << "seq,op,key,latency_ns\n";
            for (uint64_t i = 0; i < ops.size(); ++i)
                tf << i << ',' << op_name(ops[i].type) << ',' << WorkloadGenerator::key_for(ops[i].key_id) << ','
                   << lat_ns[i] << '\n';
        }
    }
    fs::remove(db);
    fs::remove(KVStore::wal_path_for(db));
    return out;
}

void append_summary_csv(const std::string& path, const std::vector<RunOutput>& runs) {
    const bool need_header = !fs::exists(path) || fs::file_size(path) == 0;
    std::ofstream f(path, std::ios::app);
    if (!f) throw std::runtime_error("cannot write " + path);
    if (need_header) {
        for (size_t i = 0; i < runs[0].row.size(); ++i) f << (i ? "," : "") << runs[0].row[i].first;
        f << '\n';
    }
    for (const auto& r : runs) {
        for (size_t i = 0; i < r.row.size(); ++i) f << (i ? "," : "") << r.row[i].second;
        f << '\n';
    }
}

struct Agg {
    double mean = 0, sd = 0, min = 0, max = 0;
};
Agg aggregate(const std::vector<RunOutput>& runs, const std::string& k) {
    std::vector<double> v;
    for (const auto& r : runs) v.push_back(r.num.at(k));
    Agg a;
    a.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double acc = 0;
    for (double x : v) acc += (x - a.mean) * (x - a.mean);
    a.sd = v.size() > 1 ? std::sqrt(acc / (v.size() - 1)) : 0.0;
    a.min = *std::min_element(v.begin(), v.end());
    a.max = *std::max_element(v.begin(), v.end());
    return a;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

void write_json(const std::string& path, const std::vector<RunOutput>& runs) {
    std::ofstream f(path, std::ios::trunc);
    f << "{\n  \"runs\": [\n";
    for (size_t r = 0; r < runs.size(); ++r) {
        f << "    {";
        for (size_t i = 0; i < runs[r].row.size(); ++i) {
            const auto& [k, v] = runs[r].row[i];
            const bool numeric = !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-');
            f << (i ? ", " : "") << '"' << k << "\": " << (numeric ? v : "\"" + json_escape(v) + "\"");
        }
        f << "}" << (r + 1 < runs.size() ? "," : "") << "\n";
    }
    f << "  ],\n  \"aggregate\": {";
    bool first = true;
    for (const auto& [k, _] : runs[0].num) {
        Agg a = aggregate(runs, k);
        f << (first ? "\n" : ",\n") << "    \"" << k << "\": {\"mean\": " << str(a.mean) << ", \"sd\": " << str(a.sd)
          << ", \"min\": " << str(a.min) << ", \"max\": " << str(a.max) << "}";
        first = false;
    }
    f << "\n  }\n}\n";
}

[[noreturn]] void usage(int code) {
    std::cout <<
        R"(bench_runner — BufferScope workload runner

Workload:   --pattern=sequential|uniform|zipf|hotspot   --keys=N  --ops=N  --value-size=N
            --read-pct=N | --mix=R/W (e.g. 80/20)       --delete-pct=N (of writes)
            --zipf-theta=F  --hot-keys=F --hot-ops=F (hotspot, e.g. 0.2/0.8 or 0.05/0.95)
            --scramble  --seed=N  --warmup-ops=N
Storage:    --pool=FRAMES | --pool-mb=MiB   --io-mode=buffered|direct
            --durability=off|sync|group  --group-commit=N  --replacer=clock|lru
Protocol:   --cache=warm|oswarm|cold  --runs=N  --dir=PATH
Output:     --label=S  --summary=CSV  --json=PATH  --trace=CSV  --dump-ops=CSV  --quiet
)";
    std::exit(code);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            std::string key = a, val;
            if (auto eq = a.find('='); eq != std::string::npos) {
                key = a.substr(0, eq);
                val = a.substr(eq + 1);
            } else if (a != "--scramble" && a != "--quiet" && a != "-h" && a != "--help") {
                if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
                val = argv[++i];
            }
            auto& wl = opt.wl;
            if (key == "--pattern") wl.pattern = parse_pattern(val);
            else if (key == "--keys") wl.key_space_size = static_cast<uint32_t>(std::stoul(val));
            else if (key == "--ops") wl.num_operations = std::stoull(val);
            else if (key == "--value-size") wl.value_size = static_cast<uint32_t>(std::stoul(val));
            else if (key == "--read-pct") wl.read_pct = static_cast<uint32_t>(std::stoul(val));
            else if (key == "--mix") wl.read_pct = static_cast<uint32_t>(std::stoul(val.substr(0, val.find('/'))));
            else if (key == "--delete-pct") wl.delete_pct = static_cast<uint32_t>(std::stoul(val));
            else if (key == "--zipf-theta") wl.zipf_theta = std::stod(val);
            else if (key == "--hot-keys") wl.hot_key_fraction = std::stod(val);
            else if (key == "--hot-ops") wl.hot_op_fraction = std::stod(val);
            else if (key == "--scramble") wl.scramble = true;
            else if (key == "--seed") wl.seed = std::stoull(val);
            else if (key == "--warmup-ops") opt.warmup_ops = std::stoull(val);
            else if (key == "--pool") opt.pool_frames = std::stoul(val);
            else if (key == "--pool-mb") opt.pool_frames = static_cast<size_t>(std::stod(val) * 256.0);
            else if (key == "--io-mode") opt.io_mode = parse_io_mode(val);
            else if (key == "--durability") opt.durability = parse_durability(val);
            else if (key == "--group-commit") opt.group_commit = static_cast<uint32_t>(std::stoul(val));
            else if (key == "--replacer") opt.replacer = parse_replacer(val);
            else if (key == "--cache") opt.cache = parse_cache(val);
            else if (key == "--runs") opt.runs = std::stoul(val);
            else if (key == "--dir") opt.dir = val;
            else if (key == "--label") opt.label = val;
            else if (key == "--summary") opt.summary_csv = val;
            else if (key == "--json") opt.json_path = val;
            else if (key == "--trace") opt.trace_path = val;
            else if (key == "--dump-ops") opt.dump_ops = val;
            else if (key == "--quiet") opt.quiet = true;
            else if (key == "-h" || key == "--help") usage(0);
            else throw std::invalid_argument("unknown option " + a);
        }
        if (opt.pool_frames < 2) opt.pool_frames = 2;
        if (opt.runs == 0) throw std::invalid_argument("--runs must be >= 1");

        WorkloadGenerator gen(opt.wl);
        auto ops = gen.generate();
        if (!opt.dump_ops.empty()) {
            gen.dump_csv(ops, opt.dump_ops);
            std::cout << "wrote " << ops.size() << " ops to " << opt.dump_ops << "\n";
            return 0;
        }
        WorkloadConfig wcfg = opt.wl;
        wcfg.num_operations = opt.warmup_ops ? opt.warmup_ops : std::max<uint64_t>(1, opt.wl.num_operations / 10);
        wcfg.seed = opt.wl.seed ^ 0xC0FFEEull;  // different ops, same distribution
        auto warm_ops = WorkloadGenerator(wcfg).generate();

        fs::create_directories(opt.dir);
        if (!opt.quiet) {
            std::cout << "BufferScope bench | label=" << opt.label << " pattern=" << pattern_name(opt.wl.pattern)
                      << " read%=" << opt.wl.read_pct << " keys=" << opt.wl.key_space_size
                      << " ops=" << opt.wl.num_operations << " pool=" << opt.pool_frames << " frames ("
                      << opt.pool_frames * 4 / 1024.0 << " MiB) io=" << io_mode_name(opt.io_mode)
                      << " durability=" << durability_name(opt.durability) << " cache=" << cache_name(opt.cache)
                      << " runs=" << opt.runs << " seed=" << opt.wl.seed << "\n";
        }

        std::vector<RunOutput> runs;
        for (size_t r = 1; r <= opt.runs; ++r) {
            runs.push_back(execute_run(opt, r, gen, opt.cache == CacheState::WARM ? warm_ops : std::vector<Operation>{},
                                       ops));
            if (!opt.quiet) {
                const auto& n = runs.back().num;
                std::cout << std::fixed << std::setprecision(2) << "  run " << r << ": " << std::setw(12)
                          << n.at("throughput_ops") << " op/s  p50 " << n.at("p50_us") << " us  p99 "
                          << n.at("p99_us") << " us  hit " << 100 * n.at("hit_ratio") << "%  io_iops "
                          << n.at("io_iops") << "  os_resident " << n.at("os_cache_resident_before") << "\n";
            }
        }

        if (!opt.quiet) {
            for (const char* k : {"throughput_ops", "p50_us", "p99_us", "hit_ratio", "io_iops"}) {
                Agg a = aggregate(runs, k);
                std::cout << std::fixed << std::setprecision(3) << "  " << std::setw(15) << k << " mean " << a.mean
                          << " sd " << a.sd << " [min " << a.min << ", max " << a.max << "]"
                          << (a.mean != 0 ? "  CoV " + str(100 * a.sd / a.mean) + "%" : "") << "\n";
            }
        }
        if (!opt.summary_csv.empty()) append_summary_csv(opt.summary_csv, runs);
        if (!opt.json_path.empty()) write_json(opt.json_path, runs);
    } catch (const std::exception& e) {
        std::cerr << "bench_runner: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
