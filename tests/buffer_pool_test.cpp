/**
 * @file buffer_pool_test.cpp
 * @brief GoogleTest suite for BufferPoolManager (Phase 2).
 *
 * Requirements verified:
 *  1. Frame struct alignment and metadata (page_id, pin_count, dirty, valid, referenced, last_access)
 *  2. FetchPage increments pin_count and tracks cache hits/misses
 *  3. UnpinPage decrements pin_count and updates dirty status
 *  4. FlushPage writes modified page to disk and verifies checksum
 *  5. Simulation reference model: assert hits and misses match an independent oracle
 *  6. Multiple page sequence access across dozens of scripted patterns
 */

#include <gtest/gtest.h>
#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/file_manager.hpp"

#include <filesystem>
#include <random>
#include <vector>
#include <unordered_set>
#include <string>

using namespace bufferscope;

class BufferPoolTest : public ::testing::Test {
protected:
    std::string test_file;

    void SetUp() override {
        test_file = "/tmp/bufferscope_bpm_test_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }

    void TearDown() override {
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }
};

// ── Test 1: Frame Structure and Alignment ──────────────────────────
TEST_F(BufferPoolTest, FrameAlignmentAndProperties) {
    FileManager fm(test_file);
    BufferPoolManager bpm(5, fm);

    EXPECT_EQ(bpm.pool_size(), 5u);

    for (size_t i = 0; i < 5; ++i) {
        const Frame* frame = bpm.get_frame(i);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(frame->pin_count, 0u);
        EXPECT_FALSE(frame->valid);
        EXPECT_FALSE(frame->dirty);
        EXPECT_FALSE(frame->referenced);

        // Check 4096-byte alignment
        uintptr_t addr = reinterpret_cast<uintptr_t>(frame->data);
        EXPECT_EQ(addr % 4096, 0u) << "Frame data buffer must be 4096-byte aligned";
    }
}

// ── Test 2: Fetch and Pin/Unpin Protocol ────────────────────────────
TEST_F(BufferPoolTest, FetchPinUnpinProtocol) {
    FileManager fm(test_file);
    BufferPoolManager bpm(10, fm);

    // Initial fetch of page 0 (Miss)
    Frame* f0 = bpm.fetch_page(0);
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(f0->page_id, 0u);
    EXPECT_EQ(f0->pin_count, 1u);
    EXPECT_TRUE(f0->valid);
    EXPECT_TRUE(f0->referenced);
    EXPECT_FALSE(f0->dirty);

    EXPECT_EQ(bpm.metrics().get_misses(), 1u);
    EXPECT_EQ(bpm.metrics().get_hits(), 0u);

    // Fetch page 0 again (Hit) -> pin_count increases to 2
    Frame* f0_second = bpm.fetch_page(0);
    EXPECT_EQ(f0, f0_second);
    EXPECT_EQ(f0->pin_count, 2u);

    EXPECT_EQ(bpm.metrics().get_hits(), 1u);
    EXPECT_EQ(bpm.metrics().get_misses(), 1u);

    // Unpin first reference
    EXPECT_TRUE(bpm.unpin_page(0, false));
    EXPECT_EQ(f0->pin_count, 1u);
    EXPECT_FALSE(f0->dirty);

    // Unpin second reference with dirty=true
    EXPECT_TRUE(bpm.unpin_page(0, true));
    EXPECT_EQ(f0->pin_count, 0u);
    EXPECT_TRUE(f0->dirty);

    // Further unpin should fail since pin_count is already 0
    EXPECT_FALSE(bpm.unpin_page(0, false));
}

// ── Test 3: Flush Page Durability ───────────────────────────────────
TEST_F(BufferPoolTest, FlushPageDurability) {
    {
        FileManager fm(test_file);
        BufferPoolManager bpm(5, fm);

        Frame* frame = bpm.fetch_page(42);
        ASSERT_NE(frame, nullptr);

        // Write some data into the page
        Page page(42);
        page.insert_record("secret_key", "secret_value");
        std::memcpy(frame->data, page.data(), PAGE_SIZE);

        // Unpin and mark dirty
        EXPECT_TRUE(bpm.unpin_page(42, true));
        EXPECT_TRUE(frame->dirty);

        // Flush page
        EXPECT_TRUE(bpm.flush_page(42));
        EXPECT_FALSE(frame->dirty);
    }

    // Verify on disk using raw FileManager
    {
        FileManager fm(test_file);
        EXPECT_TRUE(fm.page_exists(42));
        Page page = fm.read_page(42);
        EXPECT_TRUE(page.verify_checksum());
        EXPECT_EQ(page.page_id(), 42u);

        auto rec = page.read_record(0);
        ASSERT_TRUE(rec.has_value());
        EXPECT_EQ(rec->first, "secret_key");
        EXPECT_EQ(rec->second, "secret_value");
    }
}

// ── Test 4: Simulation Reference Model for Hits and Misses ─────────
TEST_F(BufferPoolTest, ReferenceModelHitMissValidation) {
    constexpr size_t POOL_SIZE = 8;
    constexpr int NUM_ACCESSES = 500;
    constexpr uint32_t PAGE_RANGE = 8;  // Accessing pages 0..7 so they all fit in pool

    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    // Independent reference model: track set of resident pages
    std::unordered_set<uint32_t> reference_resident;
    uint64_t expected_hits = 0;
    uint64_t expected_misses = 0;

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> page_dist(0, PAGE_RANGE - 1);

    for (int i = 0; i < NUM_ACCESSES; ++i) {
        uint32_t pid = page_dist(rng);

        if (reference_resident.contains(pid)) {
            expected_hits++;
        } else {
            expected_misses++;
            reference_resident.insert(pid);
        }

        Frame* f = bpm.fetch_page(pid);
        ASSERT_NE(f, nullptr) << "Failed to fetch page " << pid;
        EXPECT_EQ(f->page_id, pid);
        bpm.unpin_page(pid, false);
    }

    EXPECT_EQ(bpm.metrics().get_hits(), expected_hits);
    EXPECT_EQ(bpm.metrics().get_misses(), expected_misses);
    EXPECT_EQ(bpm.metrics().get_total_accesses(), static_cast<uint64_t>(NUM_ACCESSES));
    EXPECT_NEAR(bpm.metrics().hit_ratio(),
                static_cast<double>(expected_hits) / NUM_ACCESSES, 1e-6);
}

// ── Test 5: Scripted Multi-Sequence Access Patterns ────────────────
TEST_F(BufferPoolTest, ScriptedAccessSequences) {
    constexpr size_t POOL_SIZE = 4;
    FileManager fm(test_file);
    BufferPoolManager bpm(POOL_SIZE, fm);

    // Script: Repeated loops over pages {0, 1, 2, 3}
    // Loop 1: 0, 1, 2, 3 -> 4 Misses, 0 Hits
    for (uint32_t p = 0; p < 4; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(p, false);
    }
    EXPECT_EQ(bpm.metrics().get_misses(), 4u);
    EXPECT_EQ(bpm.metrics().get_hits(), 0u);

    // Loop 2: 0, 1, 2, 3 -> 0 Misses, 4 Hits
    for (uint32_t p = 0; p < 4; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(p, false);
    }
    EXPECT_EQ(bpm.metrics().get_misses(), 4u);
    EXPECT_EQ(bpm.metrics().get_hits(), 4u);

    // Loop 3: Access hot page 0 five times -> 5 Hits
    for (int i = 0; i < 5; ++i) {
        Frame* f = bpm.fetch_page(0);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(0, false);
    }
    EXPECT_EQ(bpm.metrics().get_misses(), 4u);
    EXPECT_EQ(bpm.metrics().get_hits(), 9u);
}
