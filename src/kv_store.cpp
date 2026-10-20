#include "bufferscope/kv_store.hpp"
#include <stdexcept>
#include <iostream>

namespace bufferscope {

KVStore::KVStore(const std::string& filepath, size_t pool_size, IOMode mode)
    : fm_(filepath, mode),
      bpm_(std::make_unique<BufferPoolManager>(pool_size, fm_)) {
    rebuild_index();
}

KVStore::~KVStore() {
    try {
        flush();
    } catch (...) {
    }
}

void KVStore::ensure_active_page() {
    uint32_t count = fm_.page_count();
    if (count == 0) {
        Page p0(0u);
        p0.update_checksum();
        fm_.write_page(p0);
        fm_.flush();
        active_page_id_ = 0;
    } else {
        active_page_id_ = count - 1;
    }
}

void KVStore::rebuild_index() {
    index_.clear();
    uint32_t count = fm_.page_count();
    if (count == 0) {
        ensure_active_page();
        return;
    }

    for (uint32_t pid = 0; pid < count; ++pid) {
        Frame* frame = bpm_->fetch_page(pid);
        if (!frame) {
            Page page = fm_.read_page(pid);
            if (!page.verify_checksum()) {
                throw std::runtime_error("KVStore: Checksum verification failed on page " +
                                         std::to_string(pid));
            }
            uint16_t num_records = page.record_count();
            for (uint16_t slot = 0; slot < num_records; ++slot) {
                auto rec = page.read_record(slot);
                if (rec.has_value()) {
                    index_.put(rec->first, {pid, slot});
                }
            }
            continue;
        }

        Page page(frame->data);
        if (!page.verify_checksum()) {
            bpm_->unpin_page(pid, false);
            throw std::runtime_error("KVStore: Checksum verification failed on page " +
                                     std::to_string(pid));
        }

        uint16_t num_records = page.record_count();
        for (uint16_t slot = 0; slot < num_records; ++slot) {
            auto rec = page.read_record(slot);
            if (rec.has_value()) {
                index_.put(rec->first, {pid, slot});
            }
        }
        bpm_->unpin_page(pid, false);
    }

    active_page_id_ = count - 1;
}

bool KVStore::put(const std::string& key, const std::string& value) {
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint16_t val_len = static_cast<uint16_t>(value.size());
    uint16_t record_size = 4 + key_len + val_len;

    if (record_size + sizeof(SlotEntry) > MAX_PAYLOAD) {
        throw std::invalid_argument("KVStore: Record size exceeds page capacity");
    }

    auto existing_loc = index_.get(key);
    if (existing_loc.has_value()) {
        Frame* old_frame = bpm_->fetch_page(existing_loc->page_id);
        if (old_frame) {
            Page old_page(old_frame->data);
            old_page.delete_record(existing_loc->slot_idx);
            old_page.update_checksum();
            std::memcpy(old_frame->data, old_page.data(), PAGE_SIZE);
            bpm_->unpin_page(existing_loc->page_id, true);
        } else {
            Page old_page = fm_.read_page(existing_loc->page_id);
            old_page.delete_record(existing_loc->slot_idx);
            old_page.update_checksum();
            fm_.write_page(old_page);
        }
    }

    Frame* active_frame = bpm_->fetch_page(active_page_id_);
    if (!active_frame) {
        uint32_t new_pid = 0;
        active_frame = bpm_->new_page(new_pid);
        if (!active_frame) throw std::runtime_error("KVStore: Buffer pool exhausted");
        active_page_id_ = new_pid;
    }

    Page active_page(active_frame->data);
    if (!active_page.insert_record(key, value)) {
        bpm_->unpin_page(active_page_id_, false);

        uint32_t new_pid = 0;
        Frame* new_frame = bpm_->new_page(new_pid);
        if (!new_frame) throw std::runtime_error("KVStore: Buffer pool exhausted");

        active_page_id_ = new_pid;
        Page new_page(new_frame->data);
        if (!new_page.insert_record(key, value)) {
            bpm_->unpin_page(active_page_id_, false);
            throw std::runtime_error("KVStore: Failed to insert record into empty page");
        }
        new_page.update_checksum();
        std::memcpy(new_frame->data, new_page.data(), PAGE_SIZE);
        uint16_t slot_idx = new_page.record_count() - 1;
        bpm_->unpin_page(active_page_id_, true);
        index_.put(key, {active_page_id_, slot_idx});
    } else {
        active_page.update_checksum();
        std::memcpy(active_frame->data, active_page.data(), PAGE_SIZE);
        uint16_t slot_idx = active_page.record_count() - 1;
        bpm_->unpin_page(active_page_id_, true);
        index_.put(key, {active_page_id_, slot_idx});
    }

    return true;
}

std::optional<std::string> KVStore::get(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc.has_value()) {
        return std::nullopt;
    }

    Frame* frame = bpm_->fetch_page(loc->page_id);
    if (!frame) {
        Page page = fm_.read_page(loc->page_id);
        if (!page.verify_checksum()) {
            throw std::runtime_error("KVStore: Checksum verification failed on page " +
                                     std::to_string(loc->page_id));
        }
        auto rec = page.read_record(loc->slot_idx);
        if (!rec.has_value() || rec->first != key) return std::nullopt;
        return rec->second;
    }

    Page page(frame->data);
    if (!page.verify_checksum()) {
        bpm_->unpin_page(loc->page_id, false);
        throw std::runtime_error("KVStore: Checksum verification failed on page " +
                                 std::to_string(loc->page_id));
    }

    auto rec = page.read_record(loc->slot_idx);
    bpm_->unpin_page(loc->page_id, false);

    if (!rec.has_value() || rec->first != key) {
        return std::nullopt;
    }

    return rec->second;
}

bool KVStore::remove(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc.has_value()) {
        return false;
    }

    Frame* frame = bpm_->fetch_page(loc->page_id);
    bool deleted = false;
    if (frame) {
        Page page(frame->data);
        deleted = page.delete_record(loc->slot_idx);
        if (deleted) {
            page.update_checksum();
            std::memcpy(frame->data, page.data(), PAGE_SIZE);
        }
        bpm_->unpin_page(loc->page_id, deleted);
    } else {
        Page page = fm_.read_page(loc->page_id);
        deleted = page.delete_record(loc->slot_idx);
        if (deleted) {
            page.update_checksum();
            fm_.write_page(page);
        }
    }

    if (deleted) {
        index_.remove(key);
    }
    return deleted;
}

void KVStore::flush() {
    bpm_->flush_all_pages();
}

}  // namespace bufferscope
