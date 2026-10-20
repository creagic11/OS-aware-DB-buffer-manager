#pragma once

#include "bufferscope/errors.hpp"
#include "bufferscope/file_manager.hpp"
#include "bufferscope/metrics.hpp"
#include "bufferscope/page.hpp"
#include "bufferscope/replacer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bufferscope {

/**
 * @brief Canonical Frame structure as specified in Section 1.4 of the specification.
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
// Consequence of the canonical layout: 32 bytes of metadata before a 4096-aligned
// array pad the struct to 8 KiB, so each 4 KiB frame costs 8 KiB of RAM (the
// metadata page is touched on construction). Memory-budgeted experiments size
// pools by sizeof(Frame), not by PAGE_SIZE. A struct-of-arrays layout (metadata
// array + separate aligned data arena) would halve this; see docs/EXPERIMENTS.md.
static_assert(sizeof(Frame) == 2 * PAGE_SIZE, "canonical Frame occupies two 4 KiB pages");

enum class ReplacerType {
    CLOCK,
    LRU
};

const char* replacer_name(ReplacerType t);
ReplacerType parse_replacer(const std::string& s);  // "clock" | "lru"

/**
 * @brief Database-managed buffer pool.
 *
 * Owns a fixed pool of frames and a page table (page_id -> frame_id).
 *
 * Invariants:
 *  - a frame with pin_count > 0 is never evicted;
 *  - a dirty victim is written back (and its checksum stamped) before its
 *    memory is reused for another page;
 *  - every page loaded from disk is checksum-verified; corruption raises
 *    ChecksumMismatchError, never silently returns bad bytes;
 *  - on any I/O failure while loading, the frame is returned to the free list
 *    (no frame leaks, page table stays consistent).
 */
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, FileManager& file_manager,
                      ReplacerType replacer_type = ReplacerType::CLOCK);
    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    /// Pin a page in memory (loading it on a miss). Returns nullptr if every frame is pinned.
    /// Pages beyond the end of the file are materialised as fresh empty pages.
    Frame* fetch_page(uint32_t page_id);

    /// Unpin a page; is_dirty marks the frame modified. Returns false on bookkeeping errors.
    bool unpin_page(uint32_t page_id, bool is_dirty);

    /// Write a resident page back if dirty (no fdatasync). Returns false if not resident.
    bool flush_page(uint32_t page_id);

    /// Write back every dirty page and fdatasync the data file.
    void flush_all_pages();

    /// Allocate a brand-new page id at the end of the file, pinned and dirty.
    Frame* new_page(uint32_t& page_id);

    /// Drop an unpinned page from the pool (writing it back first if dirty).
    bool delete_page(uint32_t page_id);

    bool is_resident(uint32_t page_id) const;

    /// Write back dirty pages and drop every unpinned frame (cold buffer pool).
    /// Returns the number of frames released. Pinned frames are kept.
    size_t evict_all();

    size_t pool_size() const { return pool_size_; }
    uint32_t num_pages() const { return num_pages_; }
    const CacheMetrics& metrics() const { return metrics_; }
    CacheMetrics& mutable_metrics() { return metrics_; }
    FileManager& file_manager() { return fm_; }

    Frame* get_frame(size_t frame_id);
    const Frame* get_frame(size_t frame_id) const;
    Replacer& replacer() { return *replacer_; }

    /// Number of frames currently pinned (diagnostics / tests).
    size_t pinned_frames() const;

    /// Write-ahead hook, called with a page's LSN right before that page is
    /// written to disk. The WAL uses it to enforce "log durable up to page LSN
    /// before the page reaches disk".
    void set_before_page_write(std::function<void(uint64_t page_lsn)> hook) {
        before_page_write_ = std::move(hook);
    }

private:
    size_t pool_size_;
    FileManager& fm_;
    std::unique_ptr<Frame[]> frames_;
    std::unordered_map<uint32_t, size_t> page_table_;
    std::vector<size_t> free_list_;
    std::unique_ptr<Replacer> replacer_;
    CacheMetrics metrics_;
    uint64_t access_counter_ = 0;
    uint32_t num_pages_ = 0;  // max(page_count on disk, highest page id handed out + 1)
    std::function<void(uint64_t)> before_page_write_;

    void write_frame_bytes(uint32_t page_id, std::byte* data);

    std::optional<size_t> find_available_frame();
    void write_back(Frame& frame);
    void release_frame(size_t frame_id);
};

}  // namespace bufferscope
