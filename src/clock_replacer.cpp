#include "bufferscope/replacer.hpp"
#include "bufferscope/buffer_pool.hpp"

namespace bufferscope {

std::optional<size_t> ClockReplacer::victim(Frame* frames, size_t num_frames) {
    if (num_frames == 0) return std::nullopt;

    // Make at most 2 full sweeps across the frames
    size_t max_checks = 2 * num_frames;

    for (size_t i = 0; i < max_checks; ++i) {
        size_t idx = clock_hand_;
        clock_hand_ = (clock_hand_ + 1) % num_frames;

        Frame& frame = frames[idx];

        // Never evict pinned frames
        if (frame.pin_count > 0) {
            continue;
        }

        // If referenced, give second chance and clear bit
        if (frame.referenced) {
            frame.referenced = false;
            continue;
        }

        // Found unpinned victim with referenced == false
        return idx;
    }

    // All frames are pinned!
    return std::nullopt;
}

std::optional<size_t> LRUReplacer::victim(Frame* frames, size_t num_frames) {
    if (num_frames == 0) return std::nullopt;

    std::optional<size_t> lru_idx = std::nullopt;
    uint64_t oldest_access = UINT64_MAX;

    for (size_t i = 0; i < num_frames; ++i) {
        if (frames[i].pin_count == 0) {
            if (frames[i].last_access < oldest_access) {
                oldest_access = frames[i].last_access;
                lru_idx = i;
            }
        }
    }

    return lru_idx;
}

}  // namespace bufferscope
