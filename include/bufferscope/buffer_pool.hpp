#pragma once

#include "bufferscope/file_manager.hpp"
#include "bufferscope/page.hpp"
#include "bufferscope/metrics.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

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

/**
 * @brief Database-managed buffer pool (Phase 2).
 *
 * Owns a fixed pool of memory frames and a page table (page_id -> frame_id).
 * Handles FetchPage, UnpinPage, FlushPage, and metrics.
 * Note: Eviction replacer is added in Phase 3. In Phase 2, a first-free-frame
 * strategy is used; if pool is exhausted, FetchPage returns nullptr.
 */
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, FileManager& file_manager);
    ~BufferPoolManager();

    // Non-copyable
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Fetch a page into a memory frame. Increments pin_count and returns the frame.
    // Returns nullptr if page not resident and no free frame is available (Phase 2 constraint).
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

private:
    size_t pool_size_;
    FileManager& fm_;
    std::unique_ptr<Frame[]> frames_;
    std::unordered_map<uint32_t, size_t> page_table_;
    std::vector<size_t> free_list_;
    CacheMetrics metrics_;
    uint64_t access_counter_ = 0;
};

}  // namespace bufferscope
