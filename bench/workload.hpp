#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace bufferscope {

enum class OpType {
    PUT,
    GET,
    DELETE
};

enum class AccessPattern {
    SEQUENTIAL,
    UNIFORM_RANDOM,
    ZIPFIAN  // Staged for Phase 7
};

struct Operation {
    OpType type;
    std::string key;
    std::string value;
};

struct WorkloadConfig {
    size_t num_operations = 50000;
    size_t key_space_size = 1000;
    size_t value_size = 64;
    double put_ratio = 0.50;      // 50% PUT
    double get_ratio = 0.40;      // 40% GET
    double delete_ratio = 0.10;   // 10% DELETE
    AccessPattern pattern = AccessPattern::UNIFORM_RANDOM;
    uint64_t seed = 42;
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(WorkloadConfig config);
    std::vector<Operation> generate();

private:
    WorkloadConfig config_;
    std::string generate_random_string(size_t len, uint64_t seed);
};

}  // namespace bufferscope
