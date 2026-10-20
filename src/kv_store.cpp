#include "bufferscope/kv_store.hpp"

#include <cstring>
#include <stdexcept>

namespace bufferscope {

namespace {

// Load a frame's bytes into a Page, mutate, and copy back. The checksum is
// stamped lazily by the buffer pool at write-back time, so no CRC is computed
// on the hot path.
template <typename Fn>
auto with_page(Frame* frame, Fn&& fn) {
    Page page(frame->data);
    auto result = fn(page);
    std::memcpy(frame->data, page.data(), PAGE_SIZE);
    return result;
}

}  // namespace

KVStore::KVStore(const std::string& filepath, size_t pool_size, IOMode mode, ReplacerType replacer)
    : fm_(filepath, mode), bpm_(std::make_unique<BufferPoolManager>(pool_size, fm_, replacer)) {
    rebuild_index();
}

KVStore::~KVStore() {
    try {
        flush();
    } catch (...) {
    }
}

Frame* KVStore::pin_or_throw(uint32_t page_id) {
    Frame* f = bpm_->fetch_page(page_id);
    if (!f) throw BufferPoolExhaustedError("KVStore: no evictable frame (all frames pinned)");
    return f;
}

Frame* KVStore::new_page_or_throw(uint32_t& page_id) {
    Frame* f = bpm_->new_page(page_id);
    if (!f) throw BufferPoolExhaustedError("KVStore: no evictable frame for a new page");
    return f;
}

void KVStore::rebuild_index() {
    index_.clear();
    const uint32_t count = fm_.page_count();
    if (count == 0) {
        uint32_t pid = 0;
        new_page_or_throw(pid);
        bpm_->unpin_page(pid, false);
        bpm_->flush_all_pages();
        active_page_id_ = pid;
        return;
    }

    for (uint32_t pid = 0; pid < count; ++pid) {
        // fetch_page verifies magic + CRC32 and throws ChecksumMismatchError.
        Frame* frame = pin_or_throw(pid);
        Page page(frame->data);
        bpm_->unpin_page(pid, false);
        for (uint16_t slot = 0; slot < page.record_count(); ++slot) {
            auto rec = page.read_record(slot);
            if (rec.has_value()) index_.put(rec->first, {pid, slot});
        }
    }
    active_page_id_ = count - 1;
}

bool KVStore::put(const std::string& key, const std::string& value) {
    if (key.size() + value.size() > MAX_KV_BYTES) {
        throw std::invalid_argument("KVStore: record of " + std::to_string(key.size() + value.size()) +
                                    " bytes exceeds single-page capacity (" +
                                    std::to_string(MAX_KV_BYTES) + ")");
    }

    // 1. Tombstone the previous version, if any.
    if (auto old = index_.get(key)) {
        Frame* f = pin_or_throw(old->page_id);
        with_page(f, [&](Page& p) { return p.delete_record(old->slot_idx); });
        bpm_->unpin_page(old->page_id, true);
        index_.remove(key);
    }

    // 2. Append to the active page, rolling over to a fresh page when full.
    Frame* f = pin_or_throw(active_page_id_);
    uint16_t slot = 0;
    bool ok = with_page(f, [&](Page& p) {
        if (!p.insert_record(key, value)) return false;
        slot = static_cast<uint16_t>(p.record_count() - 1);
        return true;
    });
    if (!ok) {
        bpm_->unpin_page(active_page_id_, false);
        uint32_t new_pid = 0;
        f = new_page_or_throw(new_pid);
        active_page_id_ = new_pid;
        ok = with_page(f, [&](Page& p) {
            if (!p.insert_record(key, value)) return false;
            slot = static_cast<uint16_t>(p.record_count() - 1);
            return true;
        });
        if (!ok) {
            bpm_->unpin_page(active_page_id_, false);
            throw std::logic_error("KVStore: record did not fit in an empty page");
        }
    }
    bpm_->unpin_page(active_page_id_, true);
    index_.put(key, {active_page_id_, slot});
    return true;
}

std::optional<std::string> KVStore::get(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc) return std::nullopt;

    Frame* f = pin_or_throw(loc->page_id);
    // Read the record straight out of the frame (checksum was verified when the
    // page was loaded from disk; in-memory frames are trusted).
    Page page(f->data);
    auto rec = page.read_record(loc->slot_idx);
    bpm_->unpin_page(loc->page_id, false);

    if (!rec || rec->first != key) return std::nullopt;
    return std::move(rec->second);
}

bool KVStore::remove(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc) return false;

    Frame* f = pin_or_throw(loc->page_id);
    bool deleted = with_page(f, [&](Page& p) { return p.delete_record(loc->slot_idx); });
    bpm_->unpin_page(loc->page_id, deleted);
    if (deleted) index_.remove(key);
    return deleted;
}

void KVStore::flush() {
    bpm_->flush_all_pages();
}

}  // namespace bufferscope
