/**
 * @file wal_recovery_test.cpp
 * @brief Phase 6: WAL format, durability ordering and recovery semantics
 *        (single-process tests; crash scenarios live in crash_injection_test.cpp).
 */

#include <gtest/gtest.h>

#include "bufferscope/crash.hpp"
#include "bufferscope/recovery.hpp"
#include "bufferscope/wal.hpp"
#include "test_util.hpp"

#include <fstream>

using namespace bufferscope;
using namespace bstest;

namespace {

void flip_byte(const std::string& path, uint64_t off) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekg(static_cast<std::streamoff>(off));
    char c = 0;
    f.get(c);
    f.seekp(static_cast<std::streamoff>(off));
    f.put(static_cast<char>(~c));
}

KVOptions sync_opts(size_t pool = 32) {
    KVOptions o;
    o.pool_size = pool;
    o.durability = Durability::SYNC;
    return o;
}

}  // namespace

TEST(WalFormat, RecordRoundTripAndLsnContinuity) {
    ScratchDir d("wal_fmt");
    const std::string wal = d.db("x.wal");
    {
        LogManager log(wal, 7);
        EXPECT_EQ(log.checkpoint_lsn(), 1u);
        EXPECT_EQ(log.checkpoint_page_count(), 7u);
        EXPECT_EQ(log.append_put(3, 0, "k", "v", false), 2u);
        EXPECT_EQ(log.append_tombstone(3, 0, true), 3u);
        std::byte img[PAGE_SIZE] = {};
        EXPECT_EQ(log.append_page_image(4, img), 4u);
        EXPECT_EQ(log.durable_lsn(), 1u);  // nothing durable until flush()
        log.flush();
        EXPECT_EQ(log.durable_lsn(), 4u);
    }
    WalScan s = LogManager::scan(wal);
    ASSERT_EQ(s.records.size(), 4u);
    EXPECT_FALSE(s.torn_tail);
    EXPECT_EQ(s.records[1].type, WalRecordType::PUT);
    EXPECT_EQ(s.records[1].key, "k");
    EXPECT_EQ(s.records[1].value, "v");
    EXPECT_EQ(s.records[1].page_id, 3u);
    EXPECT_FALSE(s.records[1].is_commit());
    EXPECT_TRUE(s.records[2].is_commit());
    EXPECT_EQ(s.records[3].value.size(), PAGE_SIZE);
    {
        LogManager log(wal, 0);  // reopen continues the sequence
        EXPECT_EQ(log.next_lsn(), 5u);
        log.checkpoint(9);       // LSNs keep increasing across checkpoints
        EXPECT_EQ(log.checkpoint_lsn(), 5u);
        EXPECT_EQ(log.append_put(0, 0, "a", "b", true), 6u);
    }
    WalScan s2 = LogManager::scan(wal);
    ASSERT_EQ(s2.records.size(), 1u);  // the un-flushed record was never durable
    EXPECT_EQ(s2.records[0].type, WalRecordType::CHECKPOINT);
    EXPECT_EQ(s2.records[0].page_id, 9u);
}

TEST(WalFormat, TornTailIsDetectedAndTruncated) {
    ScratchDir d("wal_torn");
    const std::string wal = d.db("x.wal");
    {
        LogManager log(wal, 0);
        for (int i = 0; i < 5; ++i) log.append_put(0, static_cast<uint16_t>(i), "k" + std::to_string(i), "v", true);
        log.flush();
    }
    const auto good = std::filesystem::file_size(wal);
    {
        std::ofstream f(wal, std::ios::app | std::ios::binary);
        f.write("\x40\x00\x00\x00garbage-partial-record", 26);
    }
    {
        LogManager log(wal, 0);
        EXPECT_TRUE(log.recovered().torn_tail);
        EXPECT_EQ(log.recovered().records.size(), 6u);
        EXPECT_EQ(std::filesystem::file_size(wal), good);  // tail cut on open
        log.append_put(0, 5, "k5", "v", true);
        log.flush();
    }
    WalScan s = LogManager::scan(wal);
    EXPECT_FALSE(s.torn_tail);
    EXPECT_EQ(s.records.size(), 7u);
}

TEST(WalFormat, CorruptRecordEndsTheLog) {
    ScratchDir d("wal_crc");
    const std::string wal = d.db("x.wal");
    std::vector<uint64_t> ends;
    {
        LogManager log(wal, 0);
        for (int i = 0; i < 6; ++i) {
            log.append_put(0, static_cast<uint16_t>(i), "key" + std::to_string(i), "value", true);
            log.flush();
            ends.push_back(log.file_bytes());
        }
    }
    flip_byte(wal, ends[2] + 40);  // inside record #4's payload
    WalScan s = LogManager::scan(wal);
    EXPECT_TRUE(s.torn_tail);
    EXPECT_EQ(s.records.size(), 4u);  // checkpoint + first 3 puts
    EXPECT_EQ(s.valid_bytes, ends[2]);
}

TEST(WalFormat, CorruptedLogHeadIsAnError) {
    ScratchDir d("wal_head");
    const std::string wal = d.db("x.wal");
    { LogManager log(wal, 0); }
    flip_byte(wal, 10);
    EXPECT_THROW(LogManager(wal, 0), StorageError);
}

// Durability ordering: a SYNC put is durable when it returns; the data page is not.
TEST(WalRecovery, SyncPutIsInWalBeforePageReachesDisk) {
    ScratchDir d("wal_order");
    KVStore s(d.db(), sync_opts());
    const uint64_t page_writes_before = s.file_manager().stats().page_writes;
    s.put("alpha", "1");
    EXPECT_EQ(s.wal()->durable_lsn(), s.wal()->last_lsn());
    EXPECT_EQ(s.file_manager().stats().page_writes.load(), page_writes_before);
    WalScan scan = LogManager::scan(KVStore::wal_path_for(d.db()));
    ASSERT_GE(scan.records.size(), 3u);  // checkpoint, page image, put
    EXPECT_EQ(scan.records.back().type, WalRecordType::PUT);
    EXPECT_TRUE(scan.records.back().is_commit());
}

// WAL rule on eviction: a page is written only after the WAL covers its LSN.
TEST(WalRecovery, EvictionFlushesWalUpToPageLsnFirst) {
    ScratchDir d("wal_rule");
    KVOptions o = sync_opts(2);
    o.durability = Durability::GROUP;
    o.group_commit_ops = 1000000;  // never flushes on its own
    KVStore s(d.db(), o);
    for (int i = 0; i < 200; ++i) s.put("k" + std::to_string(i), std::string(100, 'x'));
    // Evictions happened (pool of 2, many pages) and each forced a WAL flush.
    EXPECT_GT(s.buffer_pool().metrics().get_dirty_evictions(), 0u);
    EXPECT_GT(s.wal()->stats().flushes.load(), 0u);
    // Every page on disk has an LSN that is durable in the WAL.
    for (uint32_t p = 0; p < s.file_manager().page_count(); ++p) {
        Page pg = s.file_manager().read_page(p);
        if (pg.verify_checksum()) {
            EXPECT_LE(pg.lsn(), s.wal()->durable_lsn()) << p;
        }
    }
}

TEST(WalRecovery, CleanReopenNeedsNoRedo) {
    ScratchDir d("wal_clean");
    {
        KVStore s(d.db(), sync_opts());
        for (uint64_t i = 0; i < 300; ++i) apply(s, make_op(i));
    }
    KVStore s(d.db(), sync_opts());
    EXPECT_EQ(s.recovery_report().redo_applied, 0u);
    EXPECT_FALSE(s.recovery_report().did_work());
    EXPECT_EQ(dump(s), oracle(300));
}

TEST(WalRecovery, RedoIsIdempotent) {
    ScratchDir d("wal_idem");
    const std::string db = d.db();
    auto r = run_in_child([&](int) {
        KVStore s(db, sync_opts(256));
        for (uint64_t i = 0; i < 120; ++i) apply(s, make_op(i));
        bufferscope::die_now();
    });
    ASSERT_TRUE(r.killed);
    // Recover the same crashed image twice from independent copies, and once more
    // on top of an already-recovered data file with the original WAL restored.
    ScratchDir c1("wal_idem1"), c2("wal_idem2");
    for (auto* c : {&c1, &c2}) {
        std::filesystem::copy_file(db, c->db());
        std::filesystem::copy_file(KVStore::wal_path_for(db), KVStore::wal_path_for(c->db()));
    }
    { KVStore s(c1.db(), sync_opts()); EXPECT_EQ(dump(s), oracle(120)); }
    std::filesystem::copy_file(KVStore::wal_path_for(db), KVStore::wal_path_for(c1.db()),
                               std::filesystem::copy_options::overwrite_existing);
    {
        KVStore s(c1.db(), sync_opts());
        EXPECT_EQ(s.recovery_report().redo_applied, 0u);
        EXPECT_GT(s.recovery_report().redo_skipped, 0u);
        EXPECT_EQ(dump(s), oracle(120));
    }
    { KVStore s(c2.db(), sync_opts()); EXPECT_EQ(dump(s), oracle(120)); }
}

// Distinct failure path 1: checksum mismatch on a page that is NOT inside the crash
// window (no WAL image) must be reported, never silently accepted or "repaired".
TEST(WalRecovery, CorruptPageOutsideCrashWindowIsChecksumMismatch) {
    ScratchDir d("wal_corrupt");
    {
        KVStore s(d.db(), sync_opts());
        for (uint64_t i = 0; i < 300; ++i) apply(s, make_op(i));
    }  // clean checkpoint: WAL now holds no page images
    ASSERT_GE(std::filesystem::file_size(d.db()), 2 * PAGE_SIZE);
    flip_byte(d.db(), PAGE_SIZE + 3000);
    try {
        KVStore s(d.db(), sync_opts());
        FAIL() << "corruption was accepted";
    } catch (const ChecksumMismatchError& e) {
        EXPECT_EQ(e.page_id(), 1u);
    }
}

// Distinct failure path 2: a page that existed at the last checkpoint is missing.
TEST(WalRecovery, MissingPageIsPageNotFound) {
    ScratchDir d("wal_missing");
    uint32_t pages = 0;
    {
        KVStore s(d.db(), sync_opts());
        for (uint64_t i = 0; i < 300; ++i) apply(s, make_op(i));
        s.checkpoint();
        pages = s.file_manager().page_count();
    }
    ASSERT_GE(pages, 3u);
    std::filesystem::resize_file(d.db(), (pages - 1) * PAGE_SIZE);
    EXPECT_THROW(KVStore(d.db(), sync_opts()), PageNotFoundError);
}

// A page that is missing/torn but has a WAL image is restored, distinctly counted.
TEST(WalRecovery, TruncatedDataFileInsideCrashWindowIsRestored) {
    ScratchDir d("wal_restore");
    const std::string db = d.db();
    auto r = run_in_child([&](int) {
        KVStore s(db, sync_opts(256));
        s.checkpoint();
        for (uint64_t i = 0; i < 200; ++i) apply(s, make_op(i));
        bufferscope::die_now();
    });
    ASSERT_TRUE(r.killed);
    std::filesystem::resize_file(db, PAGE_SIZE / 2);  // lose (almost) the whole data file
    KVStore s(db, sync_opts());
    EXPECT_GE(s.recovery_report().pages_restored_missing, 1u);
    EXPECT_EQ(dump(s), oracle(200));
}

TEST(WalRecovery, DurabilityOffRefusesUncheckpointedWal) {
    ScratchDir d("wal_off");
    const std::string db = d.db();
    auto r = run_in_child([&](int) {
        KVStore s(db, sync_opts());
        s.put("x", "y");
        bufferscope::die_now();
    });
    ASSERT_TRUE(r.killed);
    KVOptions off;
    off.durability = Durability::OFF;
    EXPECT_THROW(KVStore(db, off), StorageError);
    { KVStore s(db, sync_opts()); EXPECT_EQ(s.get("x").value_or(""), "y"); }
    EXPECT_NO_THROW(KVStore(db, off));  // clean after recovery + checkpoint
}

TEST(WalRecovery, AutomaticCheckpointBoundsWalSize) {
    ScratchDir d("wal_auto");
    KVOptions o = sync_opts(8);
    o.checkpoint_wal_bytes = 64 * 1024;
    KVStore s(d.db(), o);
    for (uint64_t i = 0; i < 2000; ++i) apply(s, make_op(i));
    EXPECT_LE(s.wal()->file_bytes(), o.checkpoint_wal_bytes + 2 * PAGE_SIZE + 512);
    EXPECT_GT(s.wal()->checkpoint_lsn(), 1u);
    EXPECT_EQ(dump(s), oracle(2000));
}
