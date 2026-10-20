#pragma once

#include "bufferscope/file_manager.hpp"
#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/kv_index.hpp"
#include "bufferscope/page.hpp"

#include <string>
#include <optional>
#include <memory>

namespace bufferscope {

/**
 * @brief Disk-backed key-value store.
 *
 * In Phase 1, operated directly on FileManager.
 * In Phase 2, rewired to operate through BufferPoolManager.
 */
class KVStore {
public:
    explicit KVStore(const std::string& filepath, size_t pool_size = 128);
    ~KVStore();

    // Non-copyable
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    // Store a key-value pair. Overwrites if key exists.
    bool put(const std::string& key, const std::string& value);

    // Retrieve value for a given key. Returns std::nullopt if not found.
    std::optional<std::string> get(const std::string& key);

    // Delete a key. Returns true if key was present and deleted, false otherwise.
    bool remove(const std::string& key);

    // Force flush all dirty buffer pool pages to disk
    void flush();

    // Number of active live keys in the store
    size_t size() const { return index_.size(); }

    // Direct access to components
    FileManager& file_manager() { return fm_; }
    BufferPoolManager& buffer_pool() { return *bpm_; }

    // Rebuild in-memory index from disk file
    void rebuild_index();

private:
    FileManager fm_;
    std::unique_ptr<BufferPoolManager> bpm_;
    KVIndex index_;
    uint32_t active_page_id_ = 0;

    void ensure_active_page();
};

}  // namespace bufferscope
