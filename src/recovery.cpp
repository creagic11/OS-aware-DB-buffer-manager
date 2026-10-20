#include "bufferscope/recovery.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace bufferscope {

std::string RecoveryReport::summary() const {
    std::ostringstream o;
    o << "wal_records=" << wal_records << " committed_ops=" << committed_ops
      << " discarded=" << discarded_records << " redo_applied=" << redo_applied
      << " redo_skipped=" << redo_skipped << " pages_checked=" << pages_checked
      << " torn_repaired=" << pages_repaired_torn << " missing_restored=" << pages_restored_missing
      << " pages_written=" << pages_written << " wal_torn_tail=" << (wal_torn_tail ? 1 : 0);
    return o.str();
}

RecoveryManager::RecoveryManager(FileManager& fm, const WalScan& wal) : fm_(fm), wal_(wal) {}

RecoveryReport RecoveryManager::recover() {
    RecoveryReport rep;
    const auto& recs = wal_.records;
    rep.wal_records = recs.size();
    rep.wal_torn_tail = wal_.torn_tail;
    if (recs.empty()) return rep;

    const uint32_t ckpt_pages = recs.front().page_id;

    // ── 1. Analysis ────────────────────────────────────────────────────
    // Ops end at a COMMIT record; anything after the last COMMIT never committed.
    size_t committed_end = 1;  // index past the last committed record (skip CHECKPOINT)
    for (size_t i = 1; i < recs.size(); ++i) {
        if (recs[i].is_commit()) {
            committed_end = i + 1;
            ++rep.committed_ops;
        }
    }
    for (size_t i = committed_end; i < recs.size(); ++i)
        if (recs[i].type == WalRecordType::PUT || recs[i].type == WalRecordType::TOMBSTONE)
            ++rep.discarded_records;

    // First page image per page (the before-image of its first touch since the checkpoint).
    // Images are physical before-images, safe to use even from an uncommitted tail.
    std::map<uint32_t, const WalRecord*> image;
    uint32_t max_page_in_log = 0;
    bool any_page_in_log = false;
    for (const auto& r : recs) {
        if (r.type == WalRecordType::CHECKPOINT) continue;
        if (r.type == WalRecordType::PAGE_IMAGE) image.try_emplace(r.page_id, &r);
        max_page_in_log = std::max(max_page_in_log, r.page_id);
        any_page_in_log = true;
    }

    // ── 2. Page validation / repair ─────────────────────────────────────
    // Every PUT/TOMBSTONE in the log is preceded (same log) by a PAGE_IMAGE of its
    // page, because the first touch of a page after a checkpoint logs its image.
    // So the pages redo can modify are exactly the pages with an image.
    const uint32_t disk_pages = fm_.page_count();
    uint32_t total = std::max(disk_pages, ckpt_pages);
    if (any_page_in_log) total = std::max(total, max_page_in_log + 1);

    std::map<uint32_t, std::unique_ptr<Page>> pages;  // pages redo may modify
    std::set<uint32_t> needs_write;
    auto from_image = [&](uint32_t pid) {
        auto p = std::make_unique<Page>(reinterpret_cast<const std::byte*>(image.at(pid)->value.data()));
        p->set_page_id(pid);
        return p;
    };

    for (uint32_t pid = 0; pid < total; ++pid) {
        ++rep.pages_checked;
        const bool has_image = image.contains(pid);
        if (pid < disk_pages) {
            alignas(IO_ALIGNMENT) std::byte buf[PAGE_SIZE];
            fm_.read_raw_aligned(pid, buf);
            const bool crc_ok = verify_page_bytes(buf);
            if (crc_ok && page_id_of(buf) == pid) {
                if (has_image) pages[pid] = std::make_unique<Page>(buf);
                continue;
            }
            if (has_image) {  // torn / corrupted inside the crash window: repair
                pages[pid] = from_image(pid);
                needs_write.insert(pid);
                ++rep.pages_repaired_torn;
                continue;
            }
            if (crc_ok) {
                throw PageIdMismatchError("Recovery: page " + std::to_string(pid) + " carries page_id " +
                                              std::to_string(page_id_of(buf)),
                                          pid, page_id_of(buf));
            }
            throw ChecksumMismatchError(
                "Recovery: checksum mismatch on page " + std::to_string(pid) +
                    " and the WAL holds no page image to repair it (corruption outside a crash window)",
                pid);
        }
        // Page is missing from the data file.
        if (has_image) {
            pages[pid] = from_image(pid);
        } else if (pid < ckpt_pages) {
            throw PageNotFoundError("Recovery: page " + std::to_string(pid) +
                                        " existed at the last checkpoint but is missing from the data file "
                                        "(file truncated?) and the WAL holds no image of it",
                                    pid);
        } else {
            // Allocated after the checkpoint but its image never became durable, so
            // nothing committed ever referenced it. Keep the file hole-free.
            pages[pid] = std::make_unique<Page>(pid);
        }
        needs_write.insert(pid);
        ++rep.pages_restored_missing;
    }

    // ── 3. Redo committed operations (idempotent via page LSN) ──────────
    for (size_t i = 1; i < committed_end; ++i) {
        const WalRecord& r = recs[i];
        if (r.type != WalRecordType::PUT && r.type != WalRecordType::TOMBSTONE) continue;
        auto it = pages.find(r.page_id);
        if (it == pages.end()) {
            throw StorageError("Recovery: WAL record LSN " + std::to_string(r.lsn) + " targets page " +
                               std::to_string(r.page_id) + " which has no page image in the log");
        }
        Page& page = *it->second;
        if (page.lsn() >= r.lsn) {
            ++rep.redo_skipped;
            continue;
        }
        if (r.type == WalRecordType::PUT) {
            if (page.record_count() != r.slot || !page.insert_record(r.key, r.value)) {
                throw StorageError("Recovery: redo of PUT LSN " + std::to_string(r.lsn) + " on page " +
                                   std::to_string(r.page_id) + " slot " + std::to_string(r.slot) +
                                   " does not match page state (record_count=" +
                                   std::to_string(page.record_count()) + ")");
            }
        } else {
            page.delete_record(r.slot);  // already deleted => no-op, still idempotent
        }
        page.set_lsn(r.lsn);
        needs_write.insert(r.page_id);
        ++rep.redo_applied;
    }

    // ── 4. Write back in ascending order (keeps the file hole-free) ─────
    for (uint32_t pid : needs_write) {
        fm_.write_page(*pages.at(pid));
        ++rep.pages_written;
    }
    if (rep.pages_written) fm_.flush();
    return rep;
}

}  // namespace bufferscope
