#include "bufferscope/kv_store.hpp"

#include "bufferscope/crash.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace bufferscope {

const char* durability_name(Durability d) {
    switch (d) {
        case Durability::OFF: return "off";
        case Durability::SYNC: return "sync";
        case Durability::GROUP: return "group";
    }
    return "?";
}

Durability parse_durability(const std::string& s) {
    if (s == "off") return Durability::OFF;
    if (s == "sync") return Durability::SYNC;
    if (s == "group") return Durability::GROUP;
    throw std::invalid_argument("unknown durability '" + s + "' (expected off|sync|group)");
}

namespace {

// Copy a frame into a Page, mutate, copy back. Checksums are stamped lazily by the
// buffer pool at write-back time, so no CRC is computed on the hot path.
template <typename Fn>
auto with_page(Frame* frame, Fn&& fn) {
    Page page(frame->data);
    auto result = fn(page);
    std::memcpy(frame->data, page.data(), PAGE_SIZE);
    return result;
}

uint16_t record_count_of(const Frame* f) {
    uint16_t n;
    std::memcpy(&n, f->data + offsetof(PageHeader, record_count), sizeof(n));
    return n;
}

KVOptions legacy_options(size_t pool, IOMode mode, ReplacerType r) {
    KVOptions o;
    o.pool_size = pool;
    o.io_mode = mode;
    o.replacer = r;
    return o;
}

}  // namespace

KVStore::KVStore(const std::string& filepath, size_t pool_size, IOMode mode, ReplacerType replacer)
    : KVStore(filepath, legacy_options(pool_size, mode, replacer)) {}

KVStore::KVStore(const std::string& filepath, const KVOptions& options)
    : opts_(options), fm_(filepath, options.io_mode) {
    if (opts_.durability != Durability::OFF) {
        if (opts_.pool_size < 2) throw std::invalid_argument("KVStore: WAL mode needs pool_size >= 2");
        wal_ = std::make_unique<LogManager>(wal_path_for(filepath), fm_.page_count());
        // ── Startup recovery, before the buffer pool sees any page ──
        recovery_ = RecoveryManager(fm_, wal_->recovered()).recover();
        const bool log_has_records = wal_->recovered().records.size() > 1;
        wal_->release_recovery_records();
        if (recovery_.did_work() || log_has_records) wal_->checkpoint(fm_.page_count());
    } else if (std::filesystem::exists(wal_path_for(filepath)) &&
               LogManager::scan(wal_path_for(filepath)).records.size() > 1) {
        throw StorageError("KVStore: '" + wal_path_for(filepath) +
                           "' holds un-checkpointed WAL records; open with a WAL-enabled durability "
                           "mode to recover before using Durability::OFF");
    }

    bpm_ = std::make_unique<BufferPoolManager>(opts_.pool_size, fm_, opts_.replacer);
    if (wal_) {
        LogManager* wal = wal_.get();
        bpm_->set_before_page_write([wal](uint64_t lsn) { wal->flush_to(lsn); });
    }
    rebuild_index();
}

KVStore::~KVStore() {
    try {
        checkpoint();  // clean shutdown
    } catch (...) {
    }
}

Frame* KVStore::pin_or_throw(uint32_t page_id) {
    Frame* f = bpm_->fetch_page(page_id);
    if (!f) throw BufferPoolExhaustedError("KVStore: no evictable frame (all frames pinned)");
    return f;
}

Frame* KVStore::new_page_or_throw(uint32_t& page_id) {
    Frame* f = bpm_->new_page(page_id);
    if (!f) throw BufferPoolExhaustedError("KVStore: no evictable frame for a new page");
    return f;
}

void KVStore::log_image_if_needed(uint32_t page_id, const Frame* frame) {
    if (!wal_ || imaged_pages_.contains(page_id)) return;
    wal_->append_page_image(page_id, frame->data);
    imaged_pages_.insert(page_id);
}

void KVStore::commit_op() {
    if (!wal_) return;
    if (opts_.durability == Durability::SYNC) {
        wal_->flush();
    } else if (++ops_since_sync_ >= opts_.group_commit_ops) {
        wal_->flush();
        ops_since_sync_ = 0;
    }
    if (wal_->file_bytes() > opts_.checkpoint_wal_bytes) checkpoint();
}

void KVStore::sync() {
    if (wal_) {
        wal_->flush();
        ops_since_sync_ = 0;
    }
}

void KVStore::checkpoint() {
    if (!bpm_) return;
    if (!wal_) {
        bpm_->flush_all_pages();
        return;
    }
    wal_->flush();
    bpm_->flush_all_pages();  // before-write hook keeps the WAL rule; ends with fdatasync
    crash_point(CrashPoint::CKPT_AFTER_PAGE_FLUSH);
    wal_->checkpoint(fm_.page_count());
    imaged_pages_.clear();
    ops_since_sync_ = 0;
}

void KVStore::rebuild_index() {
    index_.clear();
    const uint32_t count = fm_.page_count();
    if (count == 0) {
        uint32_t pid = 0;
        new_page_or_throw(pid);
        bpm_->unpin_page(pid, false);
        bpm_->flush_all_pages();
        active_page_id_ = pid;
        return;
    }
    for (uint32_t pid = 0; pid < count; ++pid) {
        Frame* frame = pin_or_throw(pid);  // verifies magic + CRC32 + page id
        Page page(frame->data);
        bpm_->unpin_page(pid, false);
        for (uint16_t slot = 0; slot < page.record_count(); ++slot) {
            auto rec = page.read_record(slot);
            if (rec.has_value()) index_.put(rec->first, {pid, slot});
        }
    }
    active_page_id_ = count - 1;
}

bool KVStore::put(const std::string& key, const std::string& value) {
    if (key.size() + value.size() > MAX_KV_BYTES) {
        throw std::invalid_argument("KVStore: record of " + std::to_string(key.size() + value.size()) +
                                    " bytes exceeds single-page capacity (" +
                                    std::to_string(MAX_KV_BYTES) + ")");
    }

    // 1. Pin every page this operation touches BEFORE logging anything, so no
    //    eviction (and thus no WAL flush) can happen between its log records.
    const auto old = index_.get(key);
    Frame* old_frame = old ? pin_or_throw(old->page_id) : nullptr;

    Frame* target = nullptr;
    try {
        target = pin_or_throw(active_page_id_);
        if (!Page(target->data).can_fit(key.size(), value.size())) {
            bpm_->unpin_page(active_page_id_, false);
            target = nullptr;
            uint32_t new_pid = 0;
            target = new_page_or_throw(new_pid);
            active_page_id_ = new_pid;
        }
    } catch (...) {
        if (old_frame) bpm_->unpin_page(old->page_id, false);
        throw;
    }
    const uint32_t target_pid = active_page_id_;

    // 2. Log: before-images (first touch since checkpoint), tombstone, put+COMMIT.
    uint64_t tomb_lsn = 0, put_lsn = 0;
    const uint16_t slot = record_count_of(target);
    if (wal_) {
        if (old_frame) log_image_if_needed(old->page_id, old_frame);
        log_image_if_needed(target_pid, target);
        if (old) tomb_lsn = wal_->append_tombstone(old->page_id, old->slot_idx, false);
        put_lsn = wal_->append_put(target_pid, slot, key, value, true);
    }

    // 3. Apply to the pinned frames, stamping page LSNs.
    if (old_frame) {
        with_page(old_frame, [&](Page& p) {
            p.delete_record(old->slot_idx);
            if (wal_) p.set_lsn(tomb_lsn);
            return true;
        });
    }
    const bool ok = with_page(target, [&](Page& p) {
        if (!p.insert_record(key, value)) return false;
        if (wal_) p.set_lsn(put_lsn);
        return true;
    });
    if (old_frame) bpm_->unpin_page(old->page_id, true);
    bpm_->unpin_page(target_pid, ok);
    if (!ok) throw std::logic_error("KVStore: record did not fit after can_fit() check");
    index_.put(key, {target_pid, slot});

    // 4. Commit per durability policy.
    commit_op();
    return true;
}

std::optional<std::string> KVStore::get(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc) return std::nullopt;

    Frame* f = pin_or_throw(loc->page_id);
    Page page(f->data);  // verified when loaded from disk; in-memory frames are trusted
    auto rec = page.read_record(loc->slot_idx);
    bpm_->unpin_page(loc->page_id, false);

    if (!rec || rec->first != key) return std::nullopt;
    return std::move(rec->second);
}

bool KVStore::remove(const std::string& key) {
    auto loc = index_.get(key);
    if (!loc) return false;

    Frame* f = pin_or_throw(loc->page_id);
    uint64_t lsn = 0;
    if (wal_) {
        log_image_if_needed(loc->page_id, f);
        lsn = wal_->append_tombstone(loc->page_id, loc->slot_idx, true);
    }
    bool deleted = with_page(f, [&](Page& p) {
        bool d = p.delete_record(loc->slot_idx);
        if (wal_) p.set_lsn(lsn);
        return d;
    });
    bpm_->unpin_page(loc->page_id, true);
    if (deleted) index_.remove(key);
    commit_op();
    return deleted;
}

}  // namespace bufferscope
