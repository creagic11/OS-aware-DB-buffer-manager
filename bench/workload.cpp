#include "workload.hpp"

#include "bufferscope/page.hpp"  // crc32

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace bufferscope {

const char* pattern_name(AccessPattern p) {
    switch (p) {
        case AccessPattern::SEQUENTIAL: return "sequential";
        case AccessPattern::UNIFORM_RANDOM: return "uniform";
        case AccessPattern::ZIPFIAN: return "zipf";
        case AccessPattern::HOTSPOT: return "hotspot";
    }
    return "?";
}

AccessPattern parse_pattern(const std::string& s) {
    if (s == "sequential" || s == "seq") return AccessPattern::SEQUENTIAL;
    if (s == "uniform" || s == "random") return AccessPattern::UNIFORM_RANDOM;
    if (s == "zipf" || s == "zipfian") return AccessPattern::ZIPFIAN;
    if (s == "hotspot") return AccessPattern::HOTSPOT;
    throw std::invalid_argument("unknown pattern '" + s + "' (sequential|uniform|zipf|hotspot)");
}

const char* op_name(OpType t) {
    switch (t) {
        case OpType::PUT: return "PUT";
        case OpType::GET: return "GET";
        case OpType::DELETE: return "DELETE";
    }
    return "?";
}

__extension__ typedef unsigned __int128 u128;

uint64_t SplitMix64::next_below(uint64_t n) {
    if (n <= 1) return 0;
    // Lemire 2019: unbiased bounded integers via 128-bit multiply.
    u128 m = static_cast<u128>(next()) * n;
    uint64_t lo = static_cast<uint64_t>(m);
    if (lo < n) {
        const uint64_t threshold = (0 - n) % n;
        while (lo < threshold) {
            m = static_cast<u128>(next()) * n;
            lo = static_cast<uint64_t>(m);
        }
    }
    return static_cast<uint64_t>(m >> 64);
}

namespace {
double zeta(uint64_t n, double theta) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; ++i) sum += 1.0 / std::pow(static_cast<double>(i), theta);
    return sum;
}
}  // namespace

ZipfGenerator::ZipfGenerator(uint64_t n, double theta) : n_(n), theta_(theta) {
    if (n == 0) throw std::invalid_argument("ZipfGenerator: n must be > 0");
    if (theta <= 0.0 || theta >= 1.0) throw std::invalid_argument("ZipfGenerator: theta must be in (0,1)");
    zeta2_ = zeta(2, theta);
    zetan_ = zeta(n, theta);
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) / (1.0 - zeta2_ / zetan_);
}

uint64_t ZipfGenerator::next(SplitMix64& rng) const {
    const double u = rng.next_double();
    const double uz = u * zetan_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return n_ > 1 ? 1 : 0;
    uint64_t v = static_cast<uint64_t>(static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    return v >= n_ ? n_ - 1 : v;
}

WorkloadGenerator::WorkloadGenerator(WorkloadConfig config) : config_(config) {
    if (config_.key_space_size == 0) throw std::invalid_argument("key_space_size must be > 0");
    if (config_.read_pct > 100 || config_.delete_pct > 100) throw std::invalid_argument("percentages must be <= 100");
    if (config_.pattern == AccessPattern::HOTSPOT &&
        (config_.hot_key_fraction <= 0.0 || config_.hot_key_fraction >= 1.0 || config_.hot_op_fraction < 0.0 ||
         config_.hot_op_fraction > 1.0)) {
        throw std::invalid_argument("hotspot fractions must satisfy 0<hot_keys<1, 0<=hot_ops<=1");
    }
}

std::string WorkloadGenerator::key_for(uint32_t key_id) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%010u", key_id);
    return std::string(buf, 11);
}

uint32_t WorkloadGenerator::maybe_scramble(uint32_t k) const {
    if (!config_.scramble) return k;
    // Bijective-in-practice spread: multiply by a large odd constant mod key space via
    // a 64-bit hash, then reduce. Collisions are fine for access skew purposes.
    uint64_t x = k * 0x9E3779B97F4A7C15ull;
    x ^= x >> 32;
    return static_cast<uint32_t>(x % config_.key_space_size);
}

uint32_t WorkloadGenerator::pick_key(SplitMix64& rng, uint64_t seq, const ZipfGenerator* zipf) const {
    const uint32_t n = config_.key_space_size;
    switch (config_.pattern) {
        case AccessPattern::SEQUENTIAL:
            return static_cast<uint32_t>(seq % n);
        case AccessPattern::UNIFORM_RANDOM:
            return static_cast<uint32_t>(rng.next_below(n));
        case AccessPattern::ZIPFIAN:
            return maybe_scramble(static_cast<uint32_t>(zipf->next(rng)));
        case AccessPattern::HOTSPOT: {
            uint32_t hot = static_cast<uint32_t>(std::llround(config_.hot_key_fraction * n));
            if (hot == 0) hot = 1;
            if (hot >= n) hot = n - 1;
            uint32_t k = rng.next_double() < config_.hot_op_fraction
                             ? static_cast<uint32_t>(rng.next_below(hot))
                             : hot + static_cast<uint32_t>(rng.next_below(n - hot));
            return maybe_scramble(k);
        }
    }
    return 0;
}

std::vector<Operation> WorkloadGenerator::generate() const {
    std::vector<Operation> ops;
    ops.reserve(config_.num_operations);
    SplitMix64 rng(config_.seed);
    std::unique_ptr<ZipfGenerator> zipf;
    if (config_.pattern == AccessPattern::ZIPFIAN)
        zipf = std::make_unique<ZipfGenerator>(config_.key_space_size, config_.zipf_theta);

    for (uint64_t i = 0; i < config_.num_operations; ++i) {
        Operation op{};
        op.key_id = pick_key(rng, i, zipf.get());
        const uint64_t r = rng.next_below(100);
        if (r < config_.read_pct) {
            op.type = OpType::GET;
        } else {
            op.type = (config_.delete_pct > 0 && rng.next_below(100) < config_.delete_pct) ? OpType::DELETE
                                                                                          : OpType::PUT;
        }
        ops.push_back(op);
    }
    return ops;
}

void WorkloadGenerator::fill_value(uint64_t seq, uint32_t key_id, std::string& out) const {
    out.resize(config_.value_size);
    SplitMix64 r(config_.seed ^ (seq * 0xD6E8FEB86659FD93ull) ^ key_id);
    static constexpr char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t i = 0;
    while (i < out.size()) {
        uint64_t x = r.next();
        for (int b = 0; b < 8 && i < out.size(); ++b, ++i, x >>= 8) out[i] = charset[(x & 0xFF) % 62];
    }
}

std::string WorkloadGenerator::value_for(uint64_t seq, uint32_t key_id) const {
    std::string v;
    fill_value(seq, key_id, v);
    return v;
}

void WorkloadGenerator::dump_csv(const std::vector<Operation>& ops, const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path);
    f << "seq,op,key,value_len,value_crc32\n";
    std::string v;
    for (uint64_t i = 0; i < ops.size(); ++i) {
        const Operation& op = ops[i];
        uint32_t len = 0, crc = 0;
        if (op.type == OpType::PUT) {
            fill_value(i, op.key_id, v);
            len = static_cast<uint32_t>(v.size());
            crc = crc32(reinterpret_cast<const std::byte*>(v.data()), v.size());
        }
        f << i << ',' << op_name(op.type) << ',' << key_for(op.key_id) << ',' << len << ',' << crc << '\n';
    }
}

}  // namespace bufferscope
