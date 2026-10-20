#pragma once
#include "bufferscope/kv_store.hpp"
#include <stdexcept>
#include <iostream>

namespace bufferscope {

KVStore::KVStore(const std::string& filepath)
    : fm_(filepath, IOMode::BUFFERED) {
    rebuild_index();
}

void KVStore::ensure_active_page() {
    uint32_t count = fm_.page_count();
    if (count == 0) {
        Page p0(0u);
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
    }

    active_page_id_ = count - 1;
}

bool KVStore::put(const std::string& key, const std::string& value) {
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint16_t val_len = static_cast<uint16_t>(value.size());
    uint16_t record_size = 4 + key_len + val_len;

    // Check if total record exceeds max page capacity
    if (record_size + sizeof(SlotEntry) > MAX_PAYLOAD) {
        throw std::invalid_argument("KVStore: Record size exceeds page capacity");
    }

    // If key already exists, tombstone old slot first
    auto existing_loc = index_.get(key);
    if (existing_loc.has_value()) {
        Page old_page = fm_.read_page(existing_loc->page_id);
        old_page.delete_record(existing_loc->slot_idx);
        fm_.write_page(old_page);
    }

    // Try inserting into the active page
    Page active_page = fm_.read_page(active_page_id_);
    if (!active_page.insert_record(key, value)) {
        // Active page full -> allocate new page
        active_page_id_ = fm_.page_count();
        Page new_page(active_page_id_);
        if (!new_page.insert_record(key, value)) {
            throw std::runtime_error("KVStore: Failed to insert record into empty page");
        }
        fm_.write_page(new_page);
        uint16_t slot_idx = new_page.record_count() - 1;
        index_.put(key, {active_page_id_, slot_idx});
    } else {
        // Successfully inserted into active page
        fm_.write_page(active_page);
        uint16_t slot_idx = active_page.record_count() - 1;
        index_.put(key, {active_page_id_, slot_idx});
    }

    return true;
}

std::optional<std::string> KVStore::get(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc.has_value()) {
        return std::nullopt;
    }

    Page page = fm_.read_page(loc->page_id);
    if (!page.verify_checksum()) {
        throw std::runtime_error("KVStore: Checksum verification failed on page " +
                                 std::to_string(loc->page_id));
    }

    auto rec = page.read_record(loc->slot_idx);
    if (!rec.has_value() || rec->first != key) {
        // Slot was deleted or mismatch
        return std::nullopt;
    }

    return rec->second;
}

bool KVStore::remove(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc.has_value()) {
        return false;
    }

    Page page = fm_.read_page(loc->page_id);
    bool deleted = page.delete_record(loc->slot_idx);
    if (deleted) {
        fm_.write_page(page);
        index_.remove(key);
    }
    return deleted;
}

void KVStore::flush() {
    fm_.flush();
}

}  // namespace bufferscope
