/**
 * @file file_manager_test.cpp
 * @brief GoogleTest cases for FileManager (Phase 0).
 *
 * Tests:
 *  1. Write-then-read-back returns identical page
 *  2. Non-sequential page_id seeks to correct offset
 *  3. Corrupted page fails checksum verification
 *  4. Restart test (close and reopen file)
 *  5. Page count tracking
 *  6. Multiple pages write and read
 */

#include <gtest/gtest.h>
#include "bufferscope/page.hpp"
#include "bufferscope/file_manager.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>

using namespace bufferscope;

// Test fixture that manages a temporary file
class FileManagerTest : public ::testing::Test {
protected:
    std::string test_file;

    void SetUp() override {
        // Create a unique test file in /tmp
        test_file = "/tmp/bufferscope_test_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        // Remove if leftover from a previous run
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }

    void TearDown() override {
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }
};

// ── Test: write-then-read-back ─────────────────────────────────────
TEST_F(FileManagerTest, WriteAndReadBack) {
    FileManager fm(test_file);

    Page page(0u);
    page.insert_record("hello", "world");
    fm.write_page(page);
    fm.flush();

    Page read_back = fm.read_page(0u);
    EXPECT_TRUE(read_back.verify_checksum());
    EXPECT_EQ(read_back.page_id(), 0u);
    EXPECT_EQ(read_back.record_count(), 1u);

    auto rec = read_back.read_record(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->first, "hello");
    EXPECT_EQ(rec->second, "world");
}

// ── Test: non-sequential page_id ───────────────────────────────────
TEST_F(FileManagerTest, NonSequentialPageId) {
    FileManager fm(test_file);

    // Write page 0
    Page page0(0u);
    page0.insert_record("page", "zero");
    fm.write_page(page0);

    // Write page 5 (skipping 1-4)
    Page page5(5);
    page5.insert_record("page", "five");
    fm.write_page(page5);

    fm.flush();

    // Read back page 5
    Page read5 = fm.read_page(5);
    EXPECT_TRUE(read5.verify_checksum());
    EXPECT_EQ(read5.page_id(), 5u);

    auto rec = read5.read_record(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->first, "page");
    EXPECT_EQ(rec->second, "five");

    // Page 0 should still be intact
    Page read0 = fm.read_page(0u);
    EXPECT_TRUE(read0.verify_checksum());
    auto rec0 = read0.read_record(0);
    ASSERT_TRUE(rec0.has_value());
    EXPECT_EQ(rec0->second, "zero");
}

// ── Test: corrupted page detection via file ────────────────────────
TEST_F(FileManagerTest, CorruptedPageDetection) {
    {
        FileManager fm(test_file);
        Page page(0u);
        page.insert_record("secure", "data");
        fm.write_page(page);
        fm.flush();
    }

    // Corrupt the file: flip a byte at offset 100 (in the payload area)
    {
        std::fstream f(test_file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(100);
        char c;
        f.get(c);
        f.seekp(100);
        c = ~c;
        f.put(c);
        f.flush();
    }

    // Read back and verify checksum fails
    FileManager fm(test_file);
    Page corrupted = fm.read_page(0u);
    EXPECT_FALSE(corrupted.verify_checksum())
        << "Corrupted page must fail checksum verification";
}

// ── Test: restart test (close and reopen) ──────────────────────────
TEST_F(FileManagerTest, RestartTest) {
    // Phase 1: write pages
    {
        FileManager fm(test_file);
        Page page0(0u);
        page0.insert_record("key0", "value0");
        fm.write_page(page0);

        Page page1(1);
        page1.insert_record("key1", "value1");
        fm.write_page(page1);

        Page page2(2);
        page2.insert_record("key2", "value2");
        fm.write_page(page2);

        fm.flush();
    }
    // FileManager destructor closes the file

    // Phase 2: reopen and verify
    {
        FileManager fm(test_file);
        EXPECT_GE(fm.page_count(), 3u);

        for (uint32_t i = 0; i < 3; ++i) {
            Page page = fm.read_page(i);
            EXPECT_TRUE(page.verify_checksum());
            EXPECT_EQ(page.page_id(), i);

            auto rec = page.read_record(0);
            ASSERT_TRUE(rec.has_value());
            EXPECT_EQ(rec->first, "key" + std::to_string(i));
            EXPECT_EQ(rec->second, "value" + std::to_string(i));
        }
    }
}

// ── Test: page count tracking ──────────────────────────────────────
TEST_F(FileManagerTest, PageCountTracking) {
    FileManager fm(test_file);

    EXPECT_EQ(fm.page_count(), 0u);

    Page page0(0u);
    fm.write_page(page0);
    EXPECT_EQ(fm.page_count(), 1u);

    Page page1(1);
    fm.write_page(page1);
    EXPECT_EQ(fm.page_count(), 2u);
}

// ── Test: page existence check ─────────────────────────────────────
TEST_F(FileManagerTest, PageExists) {
    FileManager fm(test_file);

    EXPECT_FALSE(fm.page_exists(0));

    Page page0(0u);
    fm.write_page(page0);

    EXPECT_TRUE(fm.page_exists(0));
    EXPECT_FALSE(fm.page_exists(1));
}

// ── Test: multiple pages ───────────────────────────────────────────
TEST_F(FileManagerTest, MultiplePages) {
    FileManager fm(test_file);

    constexpr int NUM_PAGES = 50;
    for (int i = 0; i < NUM_PAGES; ++i) {
        Page page(static_cast<uint32_t>(i));
        page.insert_record("key" + std::to_string(i),
                          "value" + std::to_string(i));
        fm.write_page(page);
    }
    fm.flush();

    EXPECT_EQ(fm.page_count(), static_cast<uint32_t>(NUM_PAGES));

    for (int i = 0; i < NUM_PAGES; ++i) {
        Page page = fm.read_page(static_cast<uint32_t>(i));
        EXPECT_TRUE(page.verify_checksum());

        auto rec = page.read_record(0);
        ASSERT_TRUE(rec.has_value());
        EXPECT_EQ(rec->first, "key" + std::to_string(i));
        EXPECT_EQ(rec->second, "value" + std::to_string(i));
    }
}
