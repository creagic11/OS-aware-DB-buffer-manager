#pragma once
/**
 * @file workload.hpp
 * @brief Deterministic workload generator (Phase 4 plumbing, Phase 7 full suite).
 *
 * Access patterns:  sequential | uniform | zipf | hotspot
 * Operation mix:    read_pct % GET, the rest writes (PUT, optionally some DELETE)
 *
 * Determinism: the generator uses its own PRNG (SplitMix64) and its own
 * distribution code — never std::uniform_*_distribution, whose output is
 * implementation-defined — so a (config, seed) pair yields a byte-identical
 * operation sequence on every compiler / standard library / platform.
 *
 * Key ids map to fixed-width keys "k%010u" so every record has the same size and
 * key id i is loaded onto page floor(i / records_per_page): the key-space order
 * IS the page order, which makes "sequential" sequential on disk and makes the
 * hotspot's hot set a contiguous range of pages (unless `scramble` is set).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace bufferscope {

enum class OpType : uint8_t { PUT = 0, GET = 1, DELETE = 2 };

enum class AccessPattern { SEQUENTIAL, UNIFORM_RANDOM, ZIPFIAN, HOTSPOT };

const char* pattern_name(AccessPattern p);
AccessPattern parse_pattern(const std::string& s);  // sequential|uniform|zipf|hotspot
const char* op_name(OpType t);

struct Operation {
    OpType type;
    uint32_t key_id;
};

struct WorkloadConfig {
    uint64_t num_operations = 50000;
    uint32_t key_space_size = 10000;
    uint32_t value_size = 100;
    uint32_t read_pct = 50;          // % of ops that are GET
    uint32_t delete_pct = 0;         // % of WRITES that are DELETE (rest are PUT)
    AccessPattern pattern = AccessPattern::UNIFORM_RANDOM;
    double zipf_theta = 0.99;        // YCSB default skew
    double hot_key_fraction = 0.2;   // hotspot: fraction of keys that are hot
    double hot_op_fraction = 0.8;    // hotspot: fraction of accesses that go to hot keys
    bool scramble = false;           // hash key ids so hot keys spread over all pages
    uint64_t seed = 42;
};

/// SplitMix64 — tiny, fast, fully specified PRNG.
class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed) : s_(seed) {}
    uint64_t next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    /// Uniform double in [0, 1) with 53 random bits.
    double next_double() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    /// Uniform integer in [0, n) — Lemire's multiply-shift with rejection (unbiased).
    uint64_t next_below(uint64_t n);

private:
    uint64_t s_;
};

/// YCSB/Gray et al. Zipfian generator over [0, n): item 0 is the most popular.
class ZipfGenerator {
public:
    ZipfGenerator(uint64_t n, double theta);
    uint64_t next(SplitMix64& rng) const;
    double theta() const { return theta_; }

private:
    uint64_t n_;
    double theta_, alpha_, zetan_, eta_, zeta2_;
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(WorkloadConfig config);

    /// Generate the full operation sequence (deterministic).
    std::vector<Operation> generate() const;

    /// Key string for a key id (fixed width).
    static std::string key_for(uint32_t key_id);
    /// Deterministic value for a write: depends only on (seed, seq, key_id).
    std::string value_for(uint64_t seq, uint32_t key_id) const;
    /// Fill `out` (resized to value_size) without allocating when capacity suffices.
    void fill_value(uint64_t seq, uint32_t key_id, std::string& out) const;

    /// CSV of the op sequence (seq,op,key,value_len,value_crc32) — used to prove determinism.
    void dump_csv(const std::vector<Operation>& ops, const std::string& path) const;

    const WorkloadConfig& config() const { return config_; }

private:
    WorkloadConfig config_;
    uint32_t pick_key(SplitMix64& rng, uint64_t seq, const ZipfGenerator* zipf) const;
    uint32_t maybe_scramble(uint32_t k) const;
};

}  // namespace bufferscope
