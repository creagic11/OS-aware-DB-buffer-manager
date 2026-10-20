/**
 * @file workload_test.cpp
 * @brief Phase 7 exit criterion: workloads are deterministic and machine-readable,
 *        and the access-pattern generators hit their intended distributions.
 */

#include <gtest/gtest.h>

#include "bufferscope/kv_store.hpp"
#include "test_util.hpp"
#include "workload.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace bufferscope;

namespace {

WorkloadConfig cfg(AccessPattern p, uint64_t ops = 200000, uint32_t keys = 10000, uint64_t seed = 42) {
    WorkloadConfig c;
    c.pattern = p;
    c.num_operations = ops;
    c.key_space_size = keys;
    c.seed = seed;
    c.read_pct = 80;
    return c;
}

// Frozen on 2026-10-20 (GCC 13 / x86-64). Integer-only paths => platform independent.
constexpr uint64_t GOLDEN_UNIFORM = 8385853247138253967ull;
constexpr uint64_t GOLDEN_HOTSPOT = 18023665181853735838ull;
constexpr uint64_t GOLDEN_SEQUENTIAL = 845864996827690290ull;

uint64_t fnv1a(const std::vector<Operation>& ops) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& op : ops) {
        for (uint64_t v : {static_cast<uint64_t>(op.type), static_cast<uint64_t>(op.key_id)}) {
            for (int b = 0; b < 8; ++b) {
                h ^= (v >> (8 * b)) & 0xFF;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(Workload, SameSeedIsByteIdenticalForEveryPattern) {
    bstest::ScratchDir d("wl_det");
    for (auto p : {AccessPattern::SEQUENTIAL, AccessPattern::UNIFORM_RANDOM, AccessPattern::ZIPFIAN,
                   AccessPattern::HOTSPOT}) {
        WorkloadConfig c = cfg(p, 20000);
        c.delete_pct = 10;
        WorkloadGenerator g1(c), g2(c);
        auto a = g1.generate(), b = g2.generate();
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            ASSERT_EQ(a[i].type, b[i].type) << pattern_name(p) << " op " << i;
            ASSERT_EQ(a[i].key_id, b[i].key_id) << pattern_name(p) << " op " << i;
        }
        g1.dump_csv(a, d.db("a.csv"));
        g2.dump_csv(b, d.db("b.csv"));
        EXPECT_EQ(slurp(d.db("a.csv")), slurp(d.db("b.csv"))) << pattern_name(p);
        EXPECT_EQ(g1.value_for(17, 5), g2.value_for(17, 5));
    }
}

TEST(Workload, DifferentSeedsDiffer) {
    auto a = WorkloadGenerator(cfg(AccessPattern::UNIFORM_RANDOM, 1000, 10000, 1)).generate();
    auto b = WorkloadGenerator(cfg(AccessPattern::UNIFORM_RANDOM, 1000, 10000, 2)).generate();
    EXPECT_NE(fnv1a(a), fnv1a(b));
}

// Golden values pin the exact sequence: the generator uses only its own integer PRNG
// for these patterns, so they must not change across compilers or platforms.
TEST(Workload, GoldenSequenceHashes) {
    EXPECT_EQ(fnv1a(WorkloadGenerator(cfg(AccessPattern::UNIFORM_RANDOM, 10000)).generate()),
              GOLDEN_UNIFORM);
    EXPECT_EQ(fnv1a(WorkloadGenerator(cfg(AccessPattern::HOTSPOT, 10000)).generate()), GOLDEN_HOTSPOT);
    EXPECT_EQ(fnv1a(WorkloadGenerator(cfg(AccessPattern::SEQUENTIAL, 10000)).generate()), GOLDEN_SEQUENTIAL);
    EXPECT_EQ(WorkloadGenerator::key_for(42), "k0000000042");
}

TEST(Workload, SequentialCyclesThroughKeySpace) {
    auto ops = WorkloadGenerator(cfg(AccessPattern::SEQUENTIAL, 25, 10)).generate();
    for (size_t i = 0; i < ops.size(); ++i) EXPECT_EQ(ops[i].key_id, i % 10);
}

TEST(Workload, ReadWriteMixMatchesConfiguration) {
    for (uint32_t read_pct : {100u, 80u, 50u, 20u, 0u}) {
        WorkloadConfig c = cfg(AccessPattern::UNIFORM_RANDOM);
        c.read_pct = read_pct;
        c.delete_pct = 10;
        auto ops = WorkloadGenerator(c).generate();
        size_t gets = 0, dels = 0;
        for (const auto& op : ops) {
            gets += op.type == OpType::GET;
            dels += op.type == OpType::DELETE;
        }
        EXPECT_NEAR(100.0 * gets / ops.size(), read_pct, 0.5) << read_pct;
        if (read_pct < 100) {
            EXPECT_NEAR(100.0 * dels / (ops.size() - gets), 10.0, 1.0) << read_pct;
        }
    }
}

// Spot-check: the hot set really receives the intended share of accesses.
TEST(Workload, HotspotDistributionMatches80_20And95_5) {
    for (auto [hk, ho] : {std::pair{0.2, 0.8}, std::pair{0.05, 0.95}}) {
        WorkloadConfig c = cfg(AccessPattern::HOTSPOT);
        c.hot_key_fraction = hk;
        c.hot_op_fraction = ho;
        auto ops = WorkloadGenerator(c).generate();
        const uint32_t hot = static_cast<uint32_t>(hk * c.key_space_size);
        size_t in_hot = 0;
        std::vector<uint32_t> freq(c.key_space_size, 0);
        for (const auto& op : ops) {
            in_hot += op.key_id < hot;
            freq[op.key_id]++;
        }
        EXPECT_NEAR(static_cast<double>(in_hot) / ops.size(), ho, 0.005) << hk << "/" << ho;
        // Within the hot and cold sets access is uniform: every key is touched.
        size_t untouched_hot = 0;
        for (uint32_t k = 0; k < hot; ++k) untouched_hot += freq[k] == 0;
        EXPECT_EQ(untouched_hot, 0u);
    }
}

TEST(Workload, ZipfIsSkewedAndMonotone) {
    WorkloadConfig c = cfg(AccessPattern::ZIPFIAN, 400000, 1000);
    c.zipf_theta = 0.99;
    auto ops = WorkloadGenerator(c).generate();
    std::vector<uint64_t> f(c.key_space_size, 0);
    for (const auto& op : ops) f[op.key_id]++;
    // Theory: P(rank 1) = 1 / zeta(n, theta).
    double zetan = 0;
    for (int i = 1; i <= 1000; ++i) zetan += 1.0 / std::pow(i, 0.99);
    EXPECT_NEAR(static_cast<double>(f[0]) / ops.size(), 1.0 / zetan, 0.01);
    EXPECT_GT(f[0], f[1]);
    EXPECT_GT(f[1], f[10]);
    EXPECT_GT(f[10], f[500]);
    uint64_t top10 = 0;
    for (int i = 0; i < 100; ++i) top10 += f[i];
    EXPECT_GT(static_cast<double>(top10) / ops.size(), 0.6);  // top 10% of keys >60% of accesses
}

TEST(Workload, BoundedIntegersAreUnbiased) {
    SplitMix64 r(7);
    std::vector<uint64_t> c(7, 0);
    const int N = 700000;
    for (int i = 0; i < N; ++i) c[r.next_below(7)]++;
    double chi2 = 0;
    for (auto x : c) chi2 += (x - N / 7.0) * (x - N / 7.0) / (N / 7.0);
    EXPECT_LT(chi2, 22.46);  // p = 0.001 critical value, 6 dof
}

TEST(Workload, InvalidConfigsRejected) {
    WorkloadConfig c = cfg(AccessPattern::HOTSPOT);
    c.hot_key_fraction = 1.5;
    EXPECT_THROW(WorkloadGenerator{c}, std::invalid_argument);
    c = cfg(AccessPattern::UNIFORM_RANDOM);
    c.read_pct = 101;
    EXPECT_THROW(WorkloadGenerator{c}, std::invalid_argument);
    EXPECT_THROW(parse_pattern("bogus"), std::invalid_argument);
}

// In-place updates keep the file size and page-level working set stable.
TEST(Workload, FixedSizeOverwritesUpdateInPlace) {
    bstest::ScratchDir d("wl_inplace");
    KVOptions o;
    o.durability = Durability::SYNC;
    o.pool_size = 16;
    KVStore s(d.db(), o);
    WorkloadGenerator g(cfg(AccessPattern::UNIFORM_RANDOM, 1, 2000));
    for (uint32_t k = 0; k < 2000; ++k) s.put(WorkloadGenerator::key_for(k), g.value_for(k, k));
    s.checkpoint();
    const uint32_t pages = s.file_manager().page_count();
    for (uint32_t i = 0; i < 20000; ++i) {
        uint32_t k = (i * 7919u) % 2000u;
        s.put(WorkloadGenerator::key_for(k), g.value_for(100000 + i, k));
    }
    EXPECT_EQ(s.file_manager().page_count(), pages);
    EXPECT_EQ(s.get(WorkloadGenerator::key_for(19999u * 7919u % 2000u)), g.value_for(100000 + 19999, 19999u * 7919u % 2000u));
    // Larger value falls back to tombstone + append (file may grow), still correct.
    s.put(WorkloadGenerator::key_for(3), std::string(500, 'Z'));
    EXPECT_EQ(s.get(WorkloadGenerator::key_for(3)).value_or(""), std::string(500, 'Z'));
}
