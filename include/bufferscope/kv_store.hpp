#pragma once

#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/file_manager.hpp"
#include "bufferscope/kv_index.hpp"
#include "bufferscope/page.hpp"

#include <memory>
#include <optional>
#include <string>

namespace bufferscope {

/**
 * @brief Disk-backed key-value store (PUT/GET/DELETE) on top of the buffer pool.
 *
 * Every page access goes through BufferPoolManager — there is no path that
 * reads or writes a data page behind the pool's back (that would let a stale
 * dirty frame overwrite newer on-disk data).
 */
class KVStore {
public:
    explicit KVStore(const std::string& filepath,
                     size_t pool_size = 128,
                     IOMode mode = IOMode::BUFFERED,
                     ReplacerType replacer = ReplacerType::CLOCK);
    ~KVStore();

    // Non-copyable and non-movable: the buffer pool holds a reference to fm_,
    // so a defaulted move would leave it dangling.
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;
    KVStore(KVStore&&) = delete;
    KVStore& operator=(KVStore&&) = delete;

    /// Store a key-value pair (overwrites). Throws std::invalid_argument if the
    /// record cannot fit in a single 4 KiB page.
    bool put(const std::string& key, const std::string& value);

    std::optional<std::string> get(const std::string& key);

    /// Returns true if the key was present and is now deleted.
    bool remove(const std::string& key);

    /// Write back all dirty pages and fdatasync.
    void flush();

    size_t size() const { return index_.size(); }

    FileManager& file_manager() { return fm_; }
    BufferPoolManager& buffer_pool() { return *bpm_; }
    IOMode io_mode() const { return fm_.mode(); }

    /// Rebuild the in-memory index by scanning every page (startup path).
    void rebuild_index();

private:
    FileManager fm_;
    std::unique_ptr<BufferPoolManager> bpm_;
    KVIndex index_;
    uint32_t active_page_id_ = 0;

    Frame* pin_or_throw(uint32_t page_id);
    Frame* new_page_or_throw(uint32_t& page_id);
};

}  // namespace bufferscope
