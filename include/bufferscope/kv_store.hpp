#pragma once

#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/file_manager.hpp"
#include "bufferscope/kv_index.hpp"
#include "bufferscope/page.hpp"
#include "bufferscope/recovery.hpp"
#include "bufferscope/wal.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

namespace bufferscope {

/**
 * Durability policy for PUT/DELETE.
 *  - SYNC  : WAL fdatasync before every PUT/DELETE returns. A returned call is durable.
 *  - GROUP : WAL fdatasync every `group_commit_ops` mutations (and on sync()/close).
 *            Higher throughput; up to group_commit_ops-1 *returned* operations can be
 *            lost in a crash — they are only durable after the next group flush.
 *  - OFF   : no WAL at all (Phase 1-5 behaviour). Only flush() makes data durable and a
 *            crash can lose or tear anything since the last flush. Used as the
 *            no-logging baseline in experiments.
 */
enum class Durability { OFF, SYNC, GROUP };

const char* durability_name(Durability d);
Durability parse_durability(const std::string& s);  // off | sync | group

struct KVOptions {
    size_t pool_size = 128;
    IOMode io_mode = IOMode::BUFFERED;
    ReplacerType replacer = ReplacerType::CLOCK;
    Durability durability = Durability::SYNC;
    uint32_t group_commit_ops = 64;
    uint64_t checkpoint_wal_bytes = 64ull << 20;  // checkpoint when the WAL exceeds this
};

/**
 * @brief Disk-backed key-value store (PUT/GET/DELETE) on top of the buffer pool,
 *        with write-ahead logging and crash recovery.
 *
 * Every page access goes through BufferPoolManager. With a WAL enabled:
 *  - each mutation logs physiological records (page, slot) before changing pages;
 *  - the first change to a page after a checkpoint logs its full before-image, so a
 *    torn page write can be repaired;
 *  - a page is never written to disk before the WAL is durable up to its LSN;
 *  - startup runs RecoveryManager, then checkpoints.
 */
class KVStore {
public:
    KVStore(const std::string& filepath, const KVOptions& options);
    explicit KVStore(const std::string& filepath,
                     size_t pool_size = 128,
                     IOMode mode = IOMode::BUFFERED,
                     ReplacerType replacer = ReplacerType::CLOCK);
    ~KVStore();

    // Non-copyable and non-movable: the buffer pool holds a reference to fm_.
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

    /// Make every completed mutation durable via the WAL (no page writes).
    void sync();
    /// Checkpoint: WAL flush, write back all dirty pages, fdatasync, truncate WAL.
    /// With Durability::OFF this is a plain flush of all pages.
    void checkpoint();
    /// Alias of checkpoint() (kept for the Phase 1-5 API).
    void flush() { checkpoint(); }

    size_t size() const { return index_.size(); }

    /// Pre-size the in-memory key index (avoids rehash memory spikes during bulk loads).
    void reserve_index(size_t n) { index_.reserve(n); }

    FileManager& file_manager() { return fm_; }
    BufferPoolManager& buffer_pool() { return *bpm_; }
    LogManager* wal() { return wal_.get(); }
    IOMode io_mode() const { return fm_.mode(); }
    const KVOptions& options() const { return opts_; }
    const RecoveryReport& recovery_report() const { return recovery_; }

    static std::string wal_path_for(const std::string& db_path) { return db_path + ".wal"; }

    /// Rebuild the in-memory index by scanning every page (startup path).
    void rebuild_index();

private:
    KVOptions opts_;
    FileManager fm_;
    std::unique_ptr<LogManager> wal_;
    std::unique_ptr<BufferPoolManager> bpm_;
    KVIndex index_;
    uint32_t active_page_id_ = 0;
    std::unordered_set<uint32_t> imaged_pages_;  // pages whose before-image is logged since the checkpoint
    uint32_t ops_since_sync_ = 0;
    RecoveryReport recovery_;

    Frame* pin_or_throw(uint32_t page_id);
    Frame* new_page_or_throw(uint32_t& page_id);
    void log_image_if_needed(uint32_t page_id, const Frame* frame);
    void commit_op();
};

}  // namespace bufferscope
