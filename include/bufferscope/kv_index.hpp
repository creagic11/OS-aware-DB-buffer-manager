#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <optional>

namespace bufferscope {

/**
 * @brief Identifies the exact location of a record within the page file.
 */
struct RecordLocation {
    uint32_t page_id = 0;
    uint16_t slot_idx = 0;

    bool operator==(const RecordLocation& other) const = default;
};

/**
 * @brief Simple in-memory hash index mapping keys to page/slot locations.
 *
 * Per Phase 1 scope, a simple hash map is used. B-tree is deliberately avoided.
 * Rebuilt on startup by scanning pages.
 */
class KVIndex {
public:
    KVIndex() = default;

    void put(const std::string& key, RecordLocation loc) {
        index_[key] = loc;
    }

    std::optional<RecordLocation> get(const std::string& key) const {
        auto it = index_.find(key);
        if (it != index_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool remove(const std::string& key) {
        return index_.erase(key) > 0;
    }

    bool contains(const std::string& key) const {
        return index_.contains(key);
    }

    size_t size() const {
        return index_.size();
    }

    void reserve(size_t n) {
        index_.reserve(n);
    }

    void clear() {
        index_.clear();
    }

    const std::unordered_map<std::string, RecordLocation>& entries() const {
        return index_;
    }

private:
    std::unordered_map<std::string, RecordLocation> index_;
};

}  // namespace bufferscope
