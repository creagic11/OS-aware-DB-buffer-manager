#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <string>

namespace bufferscope {

/**
 * @brief High-precision latency distribution tracker.
 */
class LatencyTracker {
public:
    LatencyTracker() = default;

    void reserve(size_t n) {
        samples_.reserve(n);
    }

    void record_micros(double us) {
        samples_.push_back(us);
    }

    void clear() {
        samples_.clear();
    }

    size_t count() const { return samples_.size(); }

    double min() const {
        if (samples_.empty()) return 0.0;
        return *std::min_element(samples_.begin(), samples_.end());
    }

    double max() const {
        if (samples_.empty()) return 0.0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    double mean() const {
        if (samples_.empty()) return 0.0;
        double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        return sum / samples_.size();
    }

    double stddev() const {
        if (samples_.size() < 2) return 0.0;
        double m = mean();
        double accum = 0.0;
        for (double d : samples_) {
            accum += (d - m) * (d - m);
        }
        return std::sqrt(accum / (samples_.size() - 1));
    }

    double percentile(double p) {
        if (samples_.empty()) return 0.0;
        if (p <= 0.0) return min();
        if (p >= 100.0) return max();

        size_t idx = static_cast<size_t>(std::ceil((p / 100.0) * samples_.size())) - 1;
        if (idx >= samples_.size()) idx = samples_.size() - 1;

        std::nth_element(samples_.begin(), samples_.begin() + idx, samples_.end());
        return samples_[idx];
    }

    double p50() { return percentile(50.0); }
    double p90() { return percentile(90.0); }
    double p99() { return percentile(99.0); }
    double p99_9() { return percentile(99.9); }

    const std::vector<double>& raw_samples() const { return samples_; }

private:
    std::vector<double> samples_;
};

/**
 * @brief Thread-safe counters for I/O bytes and cache performance (Phase 4).
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

    void record_bytes_read(uint64_t bytes) {
        bytes_read_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void record_bytes_written(uint64_t bytes) {
        bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
    }

    uint64_t get_hits() const {
        return hits_.load(std::memory_order_relaxed);
    }

    uint64_t get_misses() const {
        return misses_.load(std::memory_order_relaxed);
    }

    uint64_t get_bytes_read() const {
        return bytes_read_.load(std::memory_order_relaxed);
    }

    uint64_t get_bytes_written() const {
        return bytes_written_.load(std::memory_order_relaxed);
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
        bytes_read_.store(0, std::memory_order_relaxed);
        bytes_written_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> bytes_read_{0};
    std::atomic<uint64_t> bytes_written_{0};
};

}  // namespace bufferscope
