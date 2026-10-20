#pragma once
/**
 * @file wal.hpp
 * @brief Write-ahead log (Phase 6).
 *
 * Physiological redo log: every record names the page (and slot) it changes, so
 * redo is deterministic and idempotent via the page LSN stored in each page
 * header ("apply record L to page P only if P.lsn < L").
 *
 * On-disk record (little-endian, 32-byte header + payload):
 *
 *   u32 length   total record bytes including this header
 *   u32 crc      CRC32 over bytes [8, length)  (everything after this field)
 *   u64 lsn      strictly increasing by 1 per record
 *   u8  type     WalRecordType
 *   u8  flags    WAL_FLAG_COMMIT on the last record of a KV operation
 *   u16 slot     slot index (PUT / TOMBSTONE)
 *   u32 page_id  target page (CHECKPOINT: number of pages in the data file)
 *   u32 key_len
 *   u32 val_len  (PAGE_IMAGE: 4096)
 *   key bytes, value bytes
 *
 * The log always starts with a CHECKPOINT record. A record that is short, has a
 * bad CRC, or breaks the LSN sequence marks the end of the log (torn tail);
 * everything after it is ignored and truncated on the next open.
 *
 * Durability: records are buffered in user space and reach the kernel only in
 * flush(), which write()s the buffer and calls fdatasync(). A KV operation is
 * committed exactly when its COMMIT record is durable.
 */

#include "bufferscope/errors.hpp"
#include "bufferscope/page.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace bufferscope {

enum class WalRecordType : uint8_t {
    CHECKPOINT = 1,  // start of log; all pages durable; page_id = page count
    PAGE_IMAGE = 2,  // full 4 KiB before-image of a page (first touch after checkpoint / new page)
    PUT = 3,         // insert (key, value) at (page_id, slot)
    TOMBSTONE = 4,   // delete the record at (page_id, slot)
};

constexpr uint8_t WAL_FLAG_COMMIT = 0x1;

#pragma pack(push, 1)
struct WalRecordHeader {
    uint32_t length;
    uint32_t crc;
    uint64_t lsn;
    uint8_t type;
    uint8_t flags;
    uint16_t slot;
    uint32_t page_id;
    uint32_t key_len;
    uint32_t val_len;
};
#pragma pack(pop)
static_assert(sizeof(WalRecordHeader) == 32, "WAL record header must be 32 bytes");

struct WalRecord {
    uint64_t lsn = 0;
    WalRecordType type = WalRecordType::CHECKPOINT;
    uint8_t flags = 0;
    uint32_t page_id = 0;
    uint16_t slot = 0;
    std::string key;
    std::string value;          // PAGE_IMAGE: the 4096-byte image
    uint64_t end_offset = 0;    // byte offset just past this record in the log file

    bool is_commit() const { return flags & WAL_FLAG_COMMIT; }
};

struct WalScan {
    std::vector<WalRecord> records;   // valid prefix, in LSN order
    uint64_t valid_bytes = 0;         // length of the valid prefix
    uint64_t file_bytes = 0;          // file size on disk
    bool torn_tail = false;           // trailing garbage / partial record present
};

struct WalStats {
    std::atomic<uint64_t> records{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> flushes{0};       // fdatasync calls on the log
    std::atomic<uint64_t> flush_ns{0};
    std::atomic<uint64_t> page_images{0};
    void reset() { records = 0; bytes = 0; flushes = 0; flush_ns = 0; page_images = 0; }
};

class LogManager {
public:
    /// Open (creating if needed) the log. A missing/empty log is initialised with
    /// CHECKPOINT(lsn=1, page_count=initial_page_count). A torn tail is truncated.
    LogManager(const std::string& path, uint32_t initial_page_count);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    /// Parse a log file without modifying it.
    static WalScan scan(const std::string& path);

    /// Records present at open time (for recovery). Cleared by release_recovery_records().
    const WalScan& recovered() const { return recovered_; }
    void release_recovery_records() { recovered_ = WalScan{}; }

    uint64_t append_put(uint32_t page_id, uint16_t slot, const std::string& key,
                        const std::string& value, bool commit);
    uint64_t append_tombstone(uint32_t page_id, uint16_t slot, bool commit);
    uint64_t append_page_image(uint32_t page_id, const std::byte* image);

    /// Make every appended record durable (write + fdatasync). No-op if nothing pending.
    void flush();
    /// Make records up to and including `lsn` durable (WAL rule for page write-back).
    void flush_to(uint64_t lsn) {
        if (lsn > durable_lsn_) flush();
    }

    /// Atomically replace the log with a single CHECKPOINT record (write temp,
    /// fsync, rename, fsync dir). Caller guarantees all pages are durable.
    void checkpoint(uint32_t page_count);

    uint64_t next_lsn() const { return next_lsn_; }
    uint64_t last_lsn() const { return next_lsn_ - 1; }
    uint64_t durable_lsn() const { return durable_lsn_; }
    uint64_t checkpoint_lsn() const { return checkpoint_lsn_; }
    uint32_t checkpoint_page_count() const { return checkpoint_page_count_; }
    uint64_t file_bytes() const { return file_bytes_ + buffer_.size(); }
    bool has_pending() const { return !buffer_.empty(); }
    const std::string& path() const { return path_; }
    WalStats& stats() { return stats_; }

private:
    std::string path_;
    int fd_ = -1;
    uint64_t next_lsn_ = 1;
    uint64_t durable_lsn_ = 0;
    uint64_t checkpoint_lsn_ = 0;
    uint32_t checkpoint_page_count_ = 0;
    uint64_t file_bytes_ = 0;
    std::vector<std::byte> buffer_;
    WalScan recovered_;
    WalStats stats_;

    uint64_t append(WalRecordType type, uint8_t flags, uint32_t page_id, uint16_t slot,
                    const void* key, uint32_t key_len, const void* val, uint32_t val_len);
    void open_fd();
};

/// Serialize one record (exposed for tests that craft logs).
std::vector<std::byte> encode_wal_record(uint64_t lsn, WalRecordType type, uint8_t flags,
                                         uint32_t page_id, uint16_t slot, const void* key,
                                         uint32_t key_len, const void* val, uint32_t val_len);

}  // namespace bufferscope
