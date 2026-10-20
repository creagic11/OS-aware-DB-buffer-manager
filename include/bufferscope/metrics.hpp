#pragma once

#include <cstdint>
#include <atomic>

namespace bufferscope {

/**
 * @brief Thread-safe counters for buffer pool hit/miss tracking.
 */
class CacheMetrics {
public:
    CacheMetrics() = default;

    void record_hit() {
        hits_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_miss() {
        misses_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t get_hits() const {
        return hits_.load(std::memory_order_relaxed);
    }

    uint64_t get_misses() const {
        return misses_.load(std::memory_order_relaxed);
    }

    uint64_t get_total_accesses() const {
        return get_hits() + get_misses();
    }

    double hit_ratio() const {
        uint64_t total = get_total_accesses();
        return total == 0 ? 0.0 : static_cast<double>(get_hits()) / static_cast<double>(total);
    }

    void reset() {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

}  // namespace bufferscope
