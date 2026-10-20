/**
 * @file crash_injection_test.cpp
 * @brief Phase 6 exit criterion: injected crashes recover exactly the committed data.
 *
 * Each scenario runs a deterministic workload in a fork()ed child that is killed with
 * SIGKILL at a specific point of the durability protocol. The child reports every
 * acknowledged operation over a pipe. The parent then reopens the store (running
 * recovery) and requires the recovered state to equal the oracle for exactly the
 * operations that were durable — no more, no less.
 *
 * Spec crash points:
 *   (a) after WAL append/flush but before the page flush      -> WalDurableBeforePageFlush
 *   (b) before any page flush at all                            -> BeforeAnyPageFlush*
 *   (c) after page flush but before the metadata (checkpoint)   -> AfterPageFlushBeforeCheckpoint
 * plus torn page writes, un-flushed appends, group commit, and power-loss style WAL
 * truncation at every record boundary.
 */

#include <gtest/gtest.h>

#include "bufferscope/crash.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <fstream>

using namespace bufferscope;
using namespace bstest;

namespace {

class CrashTest : public ::testing::TestWithParam<IOMode> {
protected:
    KVOptions opts(size_t pool, Durability d = Durability::SYNC, uint32_t group = 8) const {
        KVOptions o;
        o.pool_size = pool;
        o.io_mode = GetParam();
        o.durability = d;
        o.group_commit_ops = group;
        return o;
    }

    // Child body: open, run `n` ops reporting each acknowledged one (1-based count),
    // optionally checkpoint, then die without any clean shutdown.
    std::function<void(int)> workload(const std::string& db, KVOptions o, uint64_t n, CrashPoint p,
                                      uint64_t nth, bool checkpoint_at_end = false) {
        return [=](int fd) {
            KVStore s(db, o);
            arm_crash(p, nth);
            for (uint64_t i = 0; i < n; ++i) {
                apply(s, make_op(i));
                report(fd, i + 1);
            }
            if (checkpoint_at_end) s.checkpoint();
            disarm_crash();
            die_now();  // never a clean close: whatever is not durable must be lost
        };
    }

    void expect_state(const std::string& db, KVOptions o, uint64_t durable_ops) {
        KVStore s(db, o);
        EXPECT_EQ(dump(s), oracle(durable_ops)) << "recovery: " << s.recovery_report().summary();
        EXPECT_EQ(s.size(), oracle(durable_ops).size());
    }
};

uint64_t acked(const ChildResult& r) { return r.messages.empty() ? 0 : r.messages.back(); }

uint64_t count_invalid_pages(const std::string& db) {
    FileManager fm(db);
    uint64_t bad = 0;
    for (uint32_t p = 0; p < fm.page_count(); ++p) {
        alignas(4096) std::byte buf[PAGE_SIZE];
        fm.read_raw_aligned(p, buf);
        bad += !verify_page_bytes(buf);
    }
    return bad;
}

}  // namespace

// (a) A mutation's WAL record becomes durable, then the process dies before the
//     call returns and before any page reaches disk. That operation was never
//     acknowledged, but it IS committed, so it must be recovered (acked + 1).
TEST_P(CrashTest, WalDurableBeforePageFlush) {
    for (uint64_t nth : {1u, 37u, 120u}) {
        ScratchDir d("crash_a");
        auto r = run_in_child(workload(d.db(), opts(64), 200, CrashPoint::WAL_AFTER_FLUSH, nth));
        ASSERT_TRUE(r.killed);
        ASSERT_LT(acked(r), 200u);
        {
            KVStore s(d.db(), opts(64));
            EXPECT_EQ(s.recovery_report().redo_skipped, 0u);  // no page had been flushed
        }
        expect_state(d.db(), opts(64), acked(r) + 1);
    }
}

// (b) Large pool: no page is ever written; the crash fires at the first page
//     write-back of the final checkpoint. Everything must come from the WAL alone.
TEST_P(CrashTest, BeforeAnyPageFlushLargePool) {
    ScratchDir d("crash_b1");
    auto r = run_in_child(workload(d.db(), opts(256), 150, CrashPoint::PAGE_BEFORE_WRITE, 1, true));
    ASSERT_TRUE(r.killed);
    EXPECT_EQ(acked(r), 150u);
    {
        KVStore s(d.db(), opts(256));
        EXPECT_GT(s.recovery_report().redo_applied, 0u);
        EXPECT_EQ(s.recovery_report().redo_skipped, 0u);  // nothing was on disk yet
    }
    expect_state(d.db(), opts(256), 150);
}

// (b') Tiny pool: the crash fires at the first eviction write-back mid-workload
//      (inside the next op's page pinning, before it logs anything).
TEST_P(CrashTest, BeforeFirstEvictionWriteTinyPool) {
    ScratchDir d("crash_b2");
    auto r = run_in_child(workload(d.db(), opts(2), 400, CrashPoint::PAGE_BEFORE_WRITE, 1));
    ASSERT_TRUE(r.killed);
    ASSERT_LT(acked(r), 400u);
    expect_state(d.db(), opts(2), acked(r));
}

// (c) Pages are durable but the checkpoint record has not replaced the WAL: redo
//     must recognise every record as already applied (page LSN) and change nothing.
TEST_P(CrashTest, AfterPageFlushBeforeCheckpoint) {
    ScratchDir d("crash_c");
    auto r = run_in_child(workload(d.db(), opts(16), 250, CrashPoint::CKPT_AFTER_PAGE_FLUSH, 1, true));
    ASSERT_TRUE(r.killed);
    EXPECT_EQ(acked(r), 250u);
    {
        KVStore s(d.db(), opts(16));
        EXPECT_EQ(s.recovery_report().redo_applied, 0u);
        EXPECT_GT(s.recovery_report().redo_skipped, 0u);
        EXPECT_EQ(dump(s), oracle(250));
    }
    expect_state(d.db(), opts(16), 250);
}

// Many crash positions across eviction write-backs, including (c) at mid-run
// automatic checkpoints: every one must recover exactly the acknowledged prefix.
TEST_P(CrashTest, SweepEvictionWriteBacks) {
    for (uint64_t nth : {2u, 5u, 11u, 23u, 47u}) {
        ScratchDir d("crash_sweep");
        auto r = run_in_child(workload(d.db(), opts(3), 600, CrashPoint::PAGE_BEFORE_WRITE, nth));
        ASSERT_TRUE(r.killed) << nth;
        expect_state(d.db(), opts(3), acked(r));
    }
}

// Torn page: only half a page reaches the kernel. If that leaves an invalid page
// (it does unless every changed byte was in the first half), recovery must detect
// the checksum mismatch and repair the page from its WAL before-image.
TEST_P(CrashTest, TornPageWriteRepairedFromImage) {
    int torn_cases = 0;
    for (uint64_t nth = 1; nth <= 12; ++nth) {
        ScratchDir d("crash_torn");
        auto r = run_in_child(workload(d.db(), opts(3), 500, CrashPoint::PAGE_TORN_WRITE, nth));
        ASSERT_TRUE(r.killed) << nth;
        const uint64_t invalid = count_invalid_pages(d.db());
        {
            KVStore s(d.db(), opts(3));
            EXPECT_EQ(s.recovery_report().pages_repaired_torn, invalid) << s.recovery_report().summary();
        }
        expect_state(d.db(), opts(3), acked(r));
        torn_cases += invalid > 0;
    }
    EXPECT_GE(torn_cases, 3);  // the scenario really produced torn pages
}

// Torn write during the checkpoint itself (the fully-buffered case).
TEST_P(CrashTest, TornPageDuringCheckpoint) {
    ScratchDir d("crash_torn_ckpt");
    auto r = run_in_child(workload(d.db(), opts(256), 300, CrashPoint::PAGE_TORN_WRITE, 2, true));
    ASSERT_TRUE(r.killed);
    expect_state(d.db(), opts(256), 300);
}

// A record appended to the WAL buffer but never flushed is lost — and the op was
// never acknowledged, so losing it is correct.
TEST_P(CrashTest, UnflushedAppendIsLost) {
    ScratchDir d("crash_append");
    auto r = run_in_child(workload(d.db(), opts(64), 100, CrashPoint::WAL_AFTER_APPEND, 60));
    ASSERT_TRUE(r.killed);
    ASSERT_LT(acked(r), 100u);
    expect_state(d.db(), opts(64), acked(r));
}

// Group commit: durability advances only at group flushes (every 8 mutations).
// The child tags each acknowledged op with whether the WAL was fully durable at
// that moment. After a crash, exactly the ops up to the last durable point survive
// — acknowledged-but-unflushed ops are lost, which is the documented trade-off.
TEST_P(CrashTest, GroupCommitLosesOnlyUnflushedTail) {
    constexpr uint64_t DURABLE = 1ull << 63;
    int mid_group = 0;
    for (uint64_t nth : {30u, 77u, 141u, 203u}) {
        ScratchDir d("crash_group");
        auto o = opts(64, Durability::GROUP, 8);
        const std::string db = d.db();
        auto r = run_in_child([&](int fd) {
            KVStore s(db, o);
            arm_crash(CrashPoint::WAL_AFTER_APPEND, nth);
            for (uint64_t i = 0; i < 300; ++i) {
                apply(s, make_op(i));
                const bool durable = s.wal()->durable_lsn() == s.wal()->last_lsn();
                report(fd, (i + 1) | (durable ? DURABLE : 0));
            }
            die_now();
        });
        ASSERT_TRUE(r.killed);
        uint64_t acked_ops = 0, durable_ops = 0;
        for (uint64_t m : r.messages) {
            acked_ops = m & ~DURABLE;
            if (m & DURABLE) durable_ops = acked_ops;
        }
        EXPECT_GE(acked_ops, durable_ops);
        mid_group += acked_ops > durable_ops;
        expect_state(db, o, durable_ops);
    }
    EXPECT_GE(mid_group, 2) << "most crash positions should land mid-group";
}

// Power-loss style: kill after N ops, then truncate the WAL at every op boundary and
// at points inside records. Recovery must restore exactly the ops whose COMMIT record
// lies entirely before the cut.
TEST_P(CrashTest, WalTruncationAtEveryBoundary) {
    ScratchDir d("crash_trunc");
    const std::string db = d.db();
    auto r = run_in_child([&](int fd) {
        KVStore s(db, opts(256));
        for (uint64_t i = 0; i < 60; ++i) {
            apply(s, make_op(i));
            report(fd, s.wal()->file_bytes());  // WAL size once op i+1 is durable
        }
        die_now();
    });
    ASSERT_TRUE(r.killed);
    ASSERT_EQ(r.messages.size(), 60u);
    const std::vector<uint64_t>& end = r.messages;

    std::vector<uint64_t> cuts;
    for (size_t i = 0; i < end.size(); i += 7) {
        cuts.push_back(end[i]);        // exactly at a boundary
        cuts.push_back(end[i] - 1);    // one byte short: op i+1 incomplete
        cuts.push_back(end[i] - 20);   // inside the record header/payload
    }
    for (uint64_t cut : cuts) {
        // The CHECKPOINT header is installed atomically (temp + fsync + rename) and
        // can never be torn by a crash; only cuts after it are realistic.
        if (cut < sizeof(WalRecordHeader)) continue;
        ScratchDir c("crash_trunc_copy");
        fs::copy_file(db, c.db());
        fs::copy_file(KVStore::wal_path_for(db), KVStore::wal_path_for(c.db()));
        fs::resize_file(KVStore::wal_path_for(c.db()), cut);
        uint64_t durable = 0;
        while (durable < end.size() && end[durable] <= cut) ++durable;
        KVStore s(c.db(), opts(256));
        EXPECT_EQ(dump(s), oracle(durable)) << "cut=" << cut << " " << s.recovery_report().summary();
        const bool at_boundary = std::find(end.begin(), end.end(), cut) != end.end();
        EXPECT_EQ(s.recovery_report().wal_torn_tail, !at_boundary) << cut;
    }
}

// Crash, recover, keep working, crash again: recovery composes.
TEST_P(CrashTest, RepeatedCrashRecoverCycles) {
    ScratchDir d("crash_cycles");
    uint64_t done = 0;
    for (int cycle = 0; cycle < 4; ++cycle) {
        const uint64_t start = done;
        auto r = run_in_child([&, start](int fd) {
            KVStore s(d.db(), opts(4));
            arm_crash(CrashPoint::PAGE_BEFORE_WRITE, 7 + cycle * 3);
            for (uint64_t i = start; i < start + 200; ++i) {
                apply(s, make_op(i));
                report(fd, i + 1);
            }
            die_now();
        });
        ASSERT_TRUE(r.killed);
        done = r.messages.empty() ? start : r.messages.back();
        expect_state(d.db(), opts(4), done);
    }
}

INSTANTIATE_TEST_SUITE_P(BothPaths, CrashTest, ::testing::Values(IOMode::BUFFERED, IOMode::DIRECT),
                         [](const auto& info) { return std::string(io_mode_name(info.param)); });
