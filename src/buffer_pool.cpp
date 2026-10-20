#include "bufferscope/buffer_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace bufferscope {

const char* replacer_name(ReplacerType t) {
    return t == ReplacerType::LRU ? "lru" : "clock";
}

ReplacerType parse_replacer(const std::string& s) {
    if (s == "clock") return ReplacerType::CLOCK;
    if (s == "lru") return ReplacerType::LRU;
    throw std::invalid_argument("unknown replacer '" + s + "' (expected clock|lru)");
}

BufferPoolManager::BufferPoolManager(size_t pool_size, FileManager& file_manager,
                                     ReplacerType replacer_type)
    : pool_size_(pool_size), fm_(file_manager), frames_(std::make_unique<Frame[]>(pool_size)) {
    if (pool_size_ == 0) throw std::invalid_argument("BufferPoolManager: pool_size must be > 0");

    if (replacer_type == ReplacerType::CLOCK) {
        replacer_ = std::make_unique<ClockReplacer>(pool_size_);
    } else {
        replacer_ = std::make_unique<LRUReplacer>(pool_size_);
    }

    free_list_.reserve(pool_size_);
    // Push in reverse so frames are handed out 0, 1, 2, ... (pop_back).
    for (size_t i = pool_size_; i-- > 0;) {
        frames_[i].valid = false;
        frames_[i].pin_count = 0;
        frames_[i].dirty = false;
        frames_[i].referenced = false;
        frames_[i].last_access = 0;
        free_list_.push_back(i);
    }
    num_pages_ = fm_.page_count();
}

BufferPoolManager::~BufferPoolManager() {
    try {
        flush_all_pages();
    } catch (...) {
    }
}

Frame* BufferPoolManager::get_frame(size_t frame_id) {
    return frame_id < pool_size_ ? &frames_[frame_id] : nullptr;
}

const Frame* BufferPoolManager::get_frame(size_t frame_id) const {
    return frame_id < pool_size_ ? &frames_[frame_id] : nullptr;
}

bool BufferPoolManager::is_resident(uint32_t page_id) const {
    return page_table_.contains(page_id);
}

size_t BufferPoolManager::pinned_frames() const {
    size_t n = 0;
    for (size_t i = 0; i < pool_size_; ++i)
        if (frames_[i].valid && frames_[i].pin_count > 0) ++n;
    return n;
}

void BufferPoolManager::write_back(Frame& frame) {
    // Checksums are maintained lazily: stamp the frame in place right before
    // it reaches disk, then hand the (4096-aligned) frame buffer straight to
    // the FileManager — zero copies, valid for both BUFFERED and DIRECT modes.
    //
    // No-holes invariant: if this page lies beyond the current end of file
    // (it was materialised by fetch_page past EOF), first write every missing
    // page in between. Otherwise the gap would read back as zeros and be
    // indistinguishable from a corrupted page.
    for (uint32_t q = fm_.page_count(); q < frame.page_id; ++q) {
        auto it = page_table_.find(q);
        if (it != page_table_.end()) {
            Frame& other = frames_[it->second];
            stamp_page_checksum(other.data);
            fm_.write_raw_aligned(q, other.data);  // keeps its dirty flag
        } else {
            alignas(IO_ALIGNMENT) std::byte empty[PAGE_SIZE];
            Page fresh(q);
            std::memcpy(empty, fresh.data(), PAGE_SIZE);
            fm_.write_raw_aligned(q, empty);
        }
        metrics_.record_bytes_written(PAGE_SIZE);
    }
    stamp_page_checksum(frame.data);
    fm_.write_raw_aligned(frame.page_id, frame.data);
    metrics_.record_bytes_written(PAGE_SIZE);
    metrics_.record_flush();
    frame.dirty = false;
}

void BufferPoolManager::release_frame(size_t frame_id) {
    Frame& f = frames_[frame_id];
    f.valid = false;
    f.dirty = false;
    f.pin_count = 0;
    f.referenced = false;
    f.last_access = 0;
    free_list_.push_back(frame_id);
}

std::optional<size_t> BufferPoolManager::find_available_frame() {
    if (!free_list_.empty()) {
        size_t frame_id = free_list_.back();
        free_list_.pop_back();
        return frame_id;
    }

    auto victim_opt = replacer_->victim(frames_.get(), pool_size_);
    if (!victim_opt.has_value()) return std::nullopt;  // every frame pinned

    size_t frame_id = *victim_opt;
    Frame& victim = frames_[frame_id];
    assert(victim.pin_count == 0);

    // Eviction invariant: a dirty victim is flushed before its memory is reused.
    // If the write-back throws, the victim stays resident and mapped (nothing lost).
    const bool was_dirty = victim.valid && victim.dirty;
    if (was_dirty) write_back(victim);

    if (victim.valid) {
        page_table_.erase(victim.page_id);
        metrics_.record_eviction(was_dirty);
    }
    victim.valid = false;
    return frame_id;
}

Frame* BufferPoolManager::fetch_page(uint32_t page_id) {
    access_counter_++;

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Frame& frame = frames_[it->second];
        frame.pin_count++;
        frame.referenced = true;
        frame.last_access = access_counter_;
        metrics_.record_hit();
        return &frame;
    }

    metrics_.record_miss();

    auto frame_id_opt = find_available_frame();
    if (!frame_id_opt.has_value()) return nullptr;

    const size_t frame_id = *frame_id_opt;
    Frame& frame = frames_[frame_id];

    try {
        if (page_id < fm_.page_count()) {
            fm_.read_raw_aligned(page_id, frame.data);
            metrics_.record_bytes_read(PAGE_SIZE);
            if (!verify_page_bytes(frame.data)) {
                throw ChecksumMismatchError(
                    "BufferPool: checksum mismatch on page " + std::to_string(page_id) +
                        " (torn write or on-disk corruption)",
                    page_id);
            }
            if (page_id_of(frame.data) != page_id) {
                throw PageIdMismatchError("BufferPool: page " + std::to_string(page_id) +
                                              " carries page_id " + std::to_string(page_id_of(frame.data)),
                                          page_id, page_id_of(frame.data));
            }
        } else {
            Page fresh(page_id);
            std::memcpy(frame.data, fresh.data(), PAGE_SIZE);
            if (page_id >= num_pages_) num_pages_ = page_id + 1;
        }
    } catch (...) {
        release_frame(frame_id);  // never leak a frame on a failed load
        throw;
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
    if (it == page_table_.end()) return false;

    Frame& frame = frames_[it->second];
    if (frame.pin_count == 0) return false;

    frame.pin_count--;
    if (is_dirty) frame.dirty = true;
    return true;
}

bool BufferPoolManager::flush_page(uint32_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    Frame& frame = frames_[it->second];
    if (frame.valid && frame.dirty) write_back(frame);
    return true;
}

void BufferPoolManager::flush_all_pages() {
    // Write back in ascending page order: sequential I/O, and it never triggers
    // the gap-fill path for a page that is itself about to be written.
    std::vector<std::pair<uint32_t, size_t>> dirty;
    for (const auto& [pid, fid] : page_table_)
        if (frames_[fid].valid && frames_[fid].dirty) dirty.emplace_back(pid, fid);
    std::sort(dirty.begin(), dirty.end());
    for (const auto& [pid, fid] : dirty) write_back(frames_[fid]);
    fm_.flush();
}

Frame* BufferPoolManager::new_page(uint32_t& page_id) {
    access_counter_++;

    auto frame_id_opt = find_available_frame();
    if (!frame_id_opt.has_value()) return nullptr;

    const size_t frame_id = *frame_id_opt;
    Frame& frame = frames_[frame_id];
    const uint32_t pid = std::max(num_pages_, fm_.page_count());

    Page fresh(pid);
    std::memcpy(frame.data, fresh.data(), PAGE_SIZE);
    try {
        // Persist the empty page immediately so the file never has holes: every
        // page id < page_count() is a real, checksummed page on disk.
        fm_.write_raw_aligned(pid, frame.data);
        metrics_.record_bytes_written(PAGE_SIZE);
    } catch (...) {
        release_frame(frame_id);
        throw;
    }

    num_pages_ = pid + 1;
    page_id = pid;
    frame.page_id = pid;
    frame.pin_count = 1;
    frame.dirty = false;
    frame.valid = true;
    frame.referenced = true;
    frame.last_access = access_counter_;
    page_table_[pid] = frame_id;
    metrics_.record_miss();
    return &frame;
}

bool BufferPoolManager::delete_page(uint32_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return true;
    Frame& frame = frames_[it->second];
    if (frame.pin_count > 0) return false;
    if (frame.dirty) write_back(frame);
    size_t fid = it->second;
    page_table_.erase(it);
    release_frame(fid);
    return true;
}

}  // namespace bufferscope
