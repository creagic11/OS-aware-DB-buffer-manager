#include "bufferscope/buffer_pool.hpp"
#include <stdexcept>
#include <cstring>
#include <cassert>

namespace bufferscope {

BufferPoolManager::BufferPoolManager(size_t pool_size, FileManager& file_manager)
    : pool_size_(pool_size),
      fm_(file_manager),
      frames_(std::make_unique<Frame[]>(pool_size)) {

    free_list_.reserve(pool_size);
    // Populate free list with all frame IDs
    for (size_t i = 0; i < pool_size_; ++i) {
        frames_[i].valid = false;
        frames_[i].pin_count = 0;
        frames_[i].dirty = false;
        frames_[i].referenced = false;
        frames_[i].last_access = 0;
        free_list_.push_back(i);
    }
}

BufferPoolManager::~BufferPoolManager() {
    try {
        flush_all_pages();
    } catch (...) {
        // Suppress exceptions in destructor
    }
}

Frame* BufferPoolManager::get_frame(size_t frame_id) {
    if (frame_id >= pool_size_) return nullptr;
    return &frames_[frame_id];
}

const Frame* BufferPoolManager::get_frame(size_t frame_id) const {
    if (frame_id >= pool_size_) return nullptr;
    return &frames_[frame_id];
}

bool BufferPoolManager::is_resident(uint32_t page_id) const {
    return page_table_.contains(page_id);
}

Frame* BufferPoolManager::fetch_page(uint32_t page_id) {
    access_counter_++;

    // 1. Check for Cache Hit
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        size_t frame_id = it->second;
        Frame& frame = frames_[frame_id];
        frame.pin_count++;
        frame.referenced = true;
        frame.last_access = access_counter_;
        metrics_.record_hit();
        return &frame;
    }

    // 2. Cache Miss
    metrics_.record_miss();

    // In Phase 2: use trivial first-free-frame allocation
    if (free_list_.empty()) {
        // Pool is exhausted, and Phase 3 replacer does not exist yet.
        return nullptr;
    }

    size_t frame_id = free_list_.back();
    free_list_.pop_back();

    Frame& frame = frames_[frame_id];

    // Read page from disk
    if (fm_.page_exists(page_id)) {
        Page page = fm_.read_page(page_id);
        std::memcpy(frame.data, page.data(), PAGE_SIZE);
    } else {
        // Brand new page initialized to zeroed page with header
        Page page(page_id);
        std::memcpy(frame.data, page.data(), PAGE_SIZE);
    }

    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.dirty = false;
    frame.valid = true;
    frame.referenced = true;
    frame.last_access = access_counter_;

    page_table_[page_id] = frame_id;
    return &frame;
}

bool BufferPoolManager::unpin_page(uint32_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    size_t frame_id = it->second;
    Frame& frame = frames_[frame_id];

    if (frame.pin_count == 0) {
        return false;  // Already unpinned
    }

    frame.pin_count--;
    if (is_dirty) {
        frame.dirty = true;
    }

    return true;
}

bool BufferPoolManager::flush_page(uint32_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    size_t frame_id = it->second;
    Frame& frame = frames_[frame_id];

    if (frame.valid && frame.dirty) {
        // Reconstruct Page from frame data and update checksum
        Page page(frame.data);
        page.update_checksum();
        // Copy back updated checksum into frame data
        std::memcpy(frame.data, page.data(), PAGE_SIZE);
        // Write to disk
        fm_.write_page(page);
        frame.dirty = false;
    }

    return true;
}

void BufferPoolManager::flush_all_pages() {
    for (const auto& [pid, fid] : page_table_) {
        Frame& frame = frames_[fid];
        if (frame.valid && frame.dirty) {
            Page page(frame.data);
            page.update_checksum();
            std::memcpy(frame.data, page.data(), PAGE_SIZE);
            fm_.write_page(page);
            frame.dirty = false;
        }
    }
    fm_.flush();
}

Frame* BufferPoolManager::new_page(uint32_t& page_id) {
    access_counter_++;

    if (free_list_.empty()) {
        return nullptr;  // Pool exhausted
    }

    size_t frame_id = free_list_.back();
    free_list_.pop_back();

    page_id = fm_.page_count();

    Frame& frame = frames_[frame_id];
    Page new_p(page_id);
    std::memcpy(frame.data, new_p.data(), PAGE_SIZE);

    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.dirty = true;
    frame.valid = true;
    frame.referenced = true;
    frame.last_access = access_counter_;

    page_table_[page_id] = frame_id;

    // Immediately persist empty page to disk to claim page_id in file
    fm_.write_page(new_p);

    metrics_.record_miss();
    return &frame;
}

}  // namespace bufferscope
