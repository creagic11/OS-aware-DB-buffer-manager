/**
 * @file buffer_pool_eviction_test.cpp
 * @brief GoogleTest suite for BufferPoolManager Eviction & Dirty Flushing (Phase 3).
 *
 * Requirements verified:
 *  1. Eviction when pool is full
 *  2. All frames pinned fails cleanly (FetchPage returns nullptr)
 *  3. Sequential scan pattern shows evictions and low hit rate
 *  4. Hot/Cold pages access pattern: hot pages protected by reference bits
 *  5. Dirty page eviction flushes modified content to disk before reuse
 */

#include <gtest/gtest.h>
#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/file_manager.hpp"

#include <filesystem>
#include <string>

using namespace bufferscope;

class BufferPoolEvictionTest : public ::testing::Test {
protected:
    std::string test_file;

    void SetUp() override {
        test_file = "/tmp/bufferscope_evict_test_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }

    void TearDown() override {
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }
};

// ── Test 1: Eviction when pool is full ─────────────────────────────
TEST_F(BufferPoolEvictionTest, EvictionOnFullPool) {
    constexpr size_t POOL_SIZE = 3;
    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    // Load pages 0, 1, 2 into the 3 frames
    for (uint32_t p = 0; p < 3; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(p, false);
    }

    EXPECT_EQ(bpm.metrics().get_misses(), 3u);
    EXPECT_EQ(bpm.metrics().get_hits(), 0u);

    // Load page 3 -> must evict a page
    Frame* f3 = bpm.fetch_page(3);
    ASSERT_NE(f3, nullptr);
    EXPECT_EQ(f3->page_id, 3u);
    bpm.unpin_page(3, false);

    EXPECT_EQ(bpm.metrics().get_misses(), 4u);
}

// ── Test 2: All frames pinned failure mode ─────────────────────────
TEST_F(BufferPoolEvictionTest, AllFramesPinnedFailsCleanly) {
    constexpr size_t POOL_SIZE = 3;
    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    std::vector<Frame*> pinned;
    // Fetch and KEEP PINNED pages 0, 1, 2
    for (uint32_t p = 0; p < 3; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        pinned.push_back(f);
    }

    // Attempting to fetch page 3 must fail cleanly (nullptr)
    Frame* f3 = bpm.fetch_page(3);
    EXPECT_EQ(f3, nullptr) << "FetchPage must return nullptr when all frames are pinned";

    // Unpin one page
    bpm.unpin_page(0, false);

    // Now fetch page 3 should succeed by evicting page 0
    f3 = bpm.fetch_page(3);
    ASSERT_NE(f3, nullptr);
    EXPECT_EQ(f3->page_id, 3u);
    bpm.unpin_page(3, false);

    // Clean up remaining pins
    bpm.unpin_page(1, false);
    bpm.unpin_page(2, false);
}

// ── Test 3: Dirty victim flushed before reuse ──────────────────────
TEST_F(BufferPoolEvictionTest, DirtyVictimFlushedBeforeReuse) {
    constexpr size_t POOL_SIZE = 2;
    FileManager fm(test_file);

    {
        BufferPoolManager bpm(POOL_SIZE, fm);

        // Fetch page 0, modify it, unpin as dirty
        Frame* f0 = bpm.fetch_page(0);
        ASSERT_NE(f0, nullptr);
        Page p0(0u);
        p0.insert_record("flushed_key", "flushed_val");
        p0.update_checksum();
        std::memcpy(f0->data, p0.data(), PAGE_SIZE);
        bpm.unpin_page(0, true);  // dirty!

        // Fetch page 1, unpin clean
        Frame* f1 = bpm.fetch_page(1);
        ASSERT_NE(f1, nullptr);
        bpm.unpin_page(1, false);

        // Now fetch page 2 -> pool full! Must evict a frame (page 0)
        // Eviction invariant requires flushing dirty page 0 to disk!
        Frame* f2 = bpm.fetch_page(2);
        ASSERT_NE(f2, nullptr);
        bpm.unpin_page(2, false);
    }

    // Read directly from disk to confirm page 0 was flushed before reuse
    Page disk_page0 = fm.read_page(0u);
    EXPECT_TRUE(disk_page0.verify_checksum());
    auto rec = disk_page0.read_record(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->first, "flushed_key");
    EXPECT_EQ(rec->second, "flushed_val");
}

// ── Test 4: Hot/Cold access pattern protects hot pages ─────────────
TEST_F(BufferPoolEvictionTest, HotPagesProtectedByClock) {
    constexpr size_t POOL_SIZE = 3;
    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    // Warm up the pool with page 0 (hot)
    Frame* f0 = bpm.fetch_page(0);
    ASSERT_NE(f0, nullptr);
    bpm.unpin_page(0, false);

    // Stream 20 cold pages through the pool, touching hot page 0 on each step
    for (uint32_t cold = 1; cold <= 20; ++cold) {
        // Access hot page 0 -> sets referenced = true
        Frame* hot = bpm.fetch_page(0);
        ASSERT_NE(hot, nullptr);
        bpm.unpin_page(0, false);

        // Access cold page
        Frame* c = bpm.fetch_page(cold);
        ASSERT_NE(c, nullptr);
        bpm.unpin_page(cold, false);
    }

    // After 20 cold pages streamed through, hot page 0 MUST still be resident!
    EXPECT_TRUE(bpm.is_resident(0));
    EXPECT_GT(bpm.metrics().get_hits(), 15u);
}

// ── Test 5: Sequential scan pattern vs Hot set ─────────────────────
TEST_F(BufferPoolEvictionTest, SequentialScanCausesEvictions) {
    constexpr size_t POOL_SIZE = 4;
    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    // Sequential scan of 20 unique pages on a 4-frame pool
    for (uint32_t p = 0; p < 20; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(p, false);
    }

    // All 20 accesses were misses (0% hit ratio)
    EXPECT_EQ(bpm.metrics().get_hits(), 0u);
    EXPECT_EQ(bpm.metrics().get_misses(), 20u);

    // Now repeated access to the last resident set
    for (int loop = 0; loop < 5; ++loop) {
        for (uint32_t p = 17; p < 20; ++p) {
            Frame* f = bpm.fetch_page(p);
            ASSERT_NE(f, nullptr);
            bpm.unpin_page(p, false);
        }
    }

    // Hit ratio increased dramatically!
    EXPECT_GT(bpm.metrics().get_hits(), 10u);
    EXPECT_GT(bpm.metrics().hit_ratio(), 0.35);
}
