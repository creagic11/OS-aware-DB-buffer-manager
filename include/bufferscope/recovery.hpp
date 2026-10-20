#pragma once
/**
 * @file recovery.hpp
 * @brief Crash recovery (Phase 6): validate pages, repair from WAL page images,
 *        redo committed operations idempotently.
 *
 * Runs before the buffer pool is created, directly against the FileManager.
 *
 *  1. Analysis — the WAL's valid prefix (torn tail already cut) is split into
 *     committed operations; trailing records without a COMMIT record belong to an
 *     operation that never committed and are discarded.
 *  2. Page validation — every page in the data file is checked (magic, CRC32,
 *     page id). Outcomes are kept distinct:
 *       * valid                      -> used as-is (its LSN tells what is applied);
 *       * checksum mismatch / torn   -> restored from the WAL page image if one
 *                                       exists, else ChecksumMismatchError;
 *       * missing (beyond EOF)       -> restored from the WAL page image if one
 *                                       exists, else PageNotFoundError when the
 *                                       last checkpoint recorded it as existing.
 *  3. Redo — committed PUT/TOMBSTONE records are applied in LSN order to pages
 *     whose LSN is older than the record (idempotent: replaying twice is a no-op).
 *  4. Write-back — repaired pages are written and fdatasync'ed. The caller then
 *     checkpoints the WAL; a crash in between simply repeats recovery.
 */

#include "bufferscope/file_manager.hpp"
#include "bufferscope/wal.hpp"

#include <cstdint>
#include <string>

namespace bufferscope {

struct RecoveryReport {
    uint64_t wal_records = 0;           // valid records found in the log
    uint64_t committed_ops = 0;         // operations with a durable COMMIT record
    uint64_t discarded_records = 0;     // records of an uncommitted trailing operation
    uint64_t redo_applied = 0;          // PUT/TOMBSTONE records applied to a page
    uint64_t redo_skipped = 0;          // records already reflected (page LSN >= record LSN)
    uint64_t pages_checked = 0;
    uint64_t pages_repaired_torn = 0;   // checksum mismatch repaired from a page image
    uint64_t pages_restored_missing = 0;// missing page restored from a page image
    uint64_t pages_written = 0;
    bool wal_torn_tail = false;

    bool did_work() const {
        return redo_applied || pages_repaired_torn || pages_restored_missing || discarded_records ||
               wal_torn_tail;
    }
    std::string summary() const;
};

class RecoveryManager {
public:
    RecoveryManager(FileManager& fm, const WalScan& wal);
    RecoveryReport recover();

private:
    FileManager& fm_;
    const WalScan& wal_;
};

}  // namespace bufferscope
