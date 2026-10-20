#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bufferscope {

struct Frame;

/**
 * @brief Abstract interface for buffer pool page replacement policies.
 */
class Replacer {
public:
    virtual ~Replacer() = default;

    // Select a victim frame index.
    // Returns std::nullopt if all unpinned frames are exhausted.
    virtual std::optional<size_t> victim(Frame* frames, size_t num_frames) = 0;

    virtual void reset() = 0;
};

/**
 * @brief CLOCK (Second Chance) page replacement policy (Phase 3).
 *
 * Scans frames circularly. Frames with pin_count > 0 are skipped.
 * Frames with referenced == true have their bit cleared (second chance).
 * The first frame with pin_count == 0 and referenced == false is evicted.
 */
class ClockReplacer : public Replacer {
public:
    explicit ClockReplacer(size_t num_frames = 0) : num_frames_(num_frames) {}

    std::optional<size_t> victim(Frame* frames, size_t num_frames) override;
    void reset() override { clock_hand_ = 0; }

    size_t clock_hand() const { return clock_hand_; }

private:
    size_t num_frames_ = 0;
    size_t clock_hand_ = 0;
};

/**
 * @brief Least Recently Used (LRU) replacer (baseline comparison).
 */
class LRUReplacer : public Replacer {
public:
    explicit LRUReplacer(size_t num_frames = 0) : num_frames_(num_frames) {}

    std::optional<size_t> victim(Frame* frames, size_t num_frames) override;
    void reset() override {}

private:
    size_t num_frames_ = 0;
};

}  // namespace bufferscope
