#pragma once

#include "bufferscope/file_manager.hpp"
#include "bufferscope/page.hpp"
#include "bufferscope/metrics.hpp"
#include "bufferscope/replacer.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <memory>

namespace bufferscope {

/**
 * @brief Canonical Frame structure as specified in Section 1.4 of specification.
 */
struct Frame {
    uint32_t page_id = 0;
    uint32_t pin_count = 0;
    bool dirty = false;
    bool valid = false;
    bool referenced = false;
    uint64_t last_access = 0;
    alignas(4096) std::byte data[PAGE_SIZE];
};

enum class ReplacerType {
    CLOCK,
    LRU
};

/**
 * @brief Database-managed buffer pool (Phase 2 & 3).
 *
 * Owns a fixed pool of memory frames and a page table (page_id -> frame_id).
 * Features full CLOCK-based eviction and dirty page writeback flushing.
 */
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, FileManager& file_manager,
                      ReplacerType replacer_type = ReplacerType::CLOCK);
    ~BufferPoolManager();

    // Non-copyable
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Fetch a page into a memory frame. Increments pin_count and returns the frame.
    // Evicts a victim if full; returns nullptr if all frames are pinned.
    Frame* fetch_page(uint32_t page_id);

    // Unpin a page frame. If is_dirty is true, marks frame as dirty.
    bool unpin_page(uint32_t page_id, bool is_dirty);

    // Flush a specific page to disk if dirty
    bool flush_page(uint32_t page_id);

    // Flush all dirty pages to disk and sync file
    void flush_all_pages();

    // Allocate a new page in the buffer pool and on disk
    Frame* new_page(uint32_t& page_id);

    // Check if a page is currently resident in the buffer pool
    bool is_resident(uint32_t page_id) const;

    // Accessors
    size_t pool_size() const { return pool_size_; }
    const CacheMetrics& metrics() const { return metrics_; }
    CacheMetrics& mutable_metrics() { return metrics_; }

    // Direct access to a frame by ID (for inspection/testing)
    Frame* get_frame(size_t frame_id);
    const Frame* get_frame(size_t frame_id) const;
    Replacer& replacer() { return *replacer_; }

private:
    size_t pool_size_;
    FileManager& fm_;
    std::unique_ptr<Frame[]> frames_;
    std::unordered_map<uint32_t, size_t> page_table_;
    std::vector<size_t> free_list_;
    std::unique_ptr<Replacer> replacer_;
    CacheMetrics metrics_;
    uint64_t access_counter_ = 0;

    // Helper to find an available frame (from free list or via eviction)
    std::optional<size_t> find_available_frame();
};

}  // namespace bufferscope
