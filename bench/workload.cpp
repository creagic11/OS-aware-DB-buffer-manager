#include "workload.hpp"
#include <random>

namespace bufferscope {

WorkloadGenerator::WorkloadGenerator(WorkloadConfig config)
    : config_(config) {}

std::string WorkloadGenerator::generate_random_string(size_t len, uint64_t seed) {
    static const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::mt19937_64 rng(seed);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        s += charset[rng() % (sizeof(charset) - 1)];
    }
    return s;
}

std::vector<Operation> WorkloadGenerator::generate() {
    std::vector<Operation> ops;
    ops.reserve(config_.num_operations);

    std::mt19937_64 rng(config_.seed);
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> key_dist(0, config_.key_space_size - 1);

    size_t seq_key_idx = 0;

    for (size_t i = 0; i < config_.num_operations; ++i) {
        // Determine Key
        size_t k_idx = 0;
        if (config_.pattern == AccessPattern::SEQUENTIAL) {
            k_idx = seq_key_idx % config_.key_space_size;
            seq_key_idx++;
        } else {
            k_idx = key_dist(rng);
        }

        std::string key = "key_" + std::to_string(k_idx);

        // Determine Op Type
        double r = op_dist(rng);
        Operation op;
        op.key = key;

        if (r < config_.put_ratio) {
            op.type = OpType::PUT;
            op.value = generate_random_string(config_.value_size, config_.seed + i);
        } else if (r < config_.put_ratio + config_.get_ratio) {
            op.type = OpType::GET;
        } else {
            op.type = OpType::DELETE;
        }

        ops.push_back(std::move(op));
    }

    return ops;
}

}  // namespace bufferscope
