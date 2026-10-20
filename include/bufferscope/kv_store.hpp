#pragma once

#include "bufferscope/file_manager.hpp"
#include "bufferscope/kv_index.hpp"
#include "bufferscope/page.hpp"

#include <string>
#include <optional>
#include <memory>

namespace bufferscope {

/**
 * @brief Minimal disk-backed key-value store (Phase 1).
 *
 * Implements GET, PUT, DELETE directly against FileManager.
 * Maintains an in-memory KVIndex, reconstructed by scanning all pages on startup.
 */
class KVStore {
public:
    explicit KVStore(const std::string& filepath);
    ~KVStore() = default;

    // Non-copyable
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    // Movable
    KVStore(KVStore&&) noexcept = default;
    KVStore& operator=(KVStore&&) noexcept = default;

    // Store a key-value pair. Overwrites if key exists.
    bool put(const std::string& key, const std::string& value);

    // Retrieve value for a given key. Returns std::nullopt if not found.
    std::optional<std::string> get(const std::string& key);

    // Delete a key. Returns true if key was present and deleted, false otherwise.
    bool remove(const std::string& key);

    // Force flush file data to disk
    void flush();

    // Number of active live keys in the store
    size_t size() const { return index_.size(); }

    // Direct access to underlying FileManager (for testing/inspection)
    FileManager& file_manager() { return fm_; }

    // Rebuild in-memory index from disk file
    void rebuild_index();

private:
    FileManager fm_;
    KVIndex index_;
    uint32_t active_page_id_ = 0;

    void ensure_active_page();
};

}  // namespace bufferscope
