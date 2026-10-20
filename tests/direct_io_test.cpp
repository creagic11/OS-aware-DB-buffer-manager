/**
 * @file direct_io_test.cpp
 * @brief GoogleTest suite for O_DIRECT Direct I/O Path (Phase 5).
 *
 * Requirements verified:
 *  1. Open with IOMode::DIRECT succeeds on supported filesystem.
 *  2. Write and read-back using O_DIRECT produces identical byte content and verified checksum.
 *  3. BufferPoolManager operates with O_DIRECT and flushes dirty pages correctly.
 *  4. KVStore runs complete GET/PUT/DELETE workload under O_DIRECT.
 *  5. Misaligned memory buffer or offset fails with clear, diagnosable EINVAL error.
 *  6. Full correctness comparison: buffered vs direct paths yield identical state.
 */

#include <gtest/gtest.h>
#include "bufferscope/file_manager.hpp"
#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/kv_store.hpp"

#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

using namespace bufferscope;

class DirectIOTest : public ::testing::Test {
protected:
    std::string test_file;

    void SetUp() override {
        test_file = "/tmp/bufferscope_direct_test_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }

    void TearDown() override {
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }
};

// ── Test 1: Open with IOMode::DIRECT ───────────────────────────────
TEST_F(DirectIOTest, OpenDirectIOMode) {
    FileManager fm(test_file, IOMode::DIRECT);
    EXPECT_EQ(fm.mode(), IOMode::DIRECT);
    EXPECT_GE(fm.fd(), 0);
}

// ── Test 2: Aligned Write and Read Back with O_DIRECT ──────────────
TEST_F(DirectIOTest, AlignedWriteAndReadBack) {
    FileManager fm(test_file, IOMode::DIRECT);

    Page page(0u);
    page.insert_record("direct_key", "direct_value");
    fm.write_page(page);
    fm.flush();

    EXPECT_EQ(fm.page_count(), 1u);

    Page read_back = fm.read_page(0u);
    EXPECT_TRUE(read_back.verify_checksum());
    EXPECT_EQ(read_back.page_id(), 0u);

    auto rec = read_back.read_record(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->first, "direct_key");
    EXPECT_EQ(rec->second, "direct_value");
}

// ── Test 3: BufferPoolManager with Direct I/O ──────────────────────
TEST_F(DirectIOTest, BufferPoolManagerDirectIO) {
    FileManager fm(test_file, IOMode::DIRECT);
    BufferPoolManager bpm(4, fm);

    // Fetch page 0
    Frame* f0 = bpm.fetch_page(0);
    ASSERT_NE(f0, nullptr);

    // Check alignment of Frame::data
    uintptr_t addr = reinterpret_cast<uintptr_t>(f0->data);
    EXPECT_EQ(addr % 4096, 0u) << "Frame data buffer must be 4096-byte aligned for O_DIRECT";

    Page p0(0u);
    p0.insert_record("frame_key", "frame_val");
    p0.update_checksum();
    std::memcpy(f0->data, p0.data(), PAGE_SIZE);

    bpm.unpin_page(0, true);
    bpm.flush_all_pages();

    // Verify raw on disk via separate direct file manager
    FileManager verify_fm(test_file, IOMode::DIRECT);
    Page disk_page = verify_fm.read_page(0u);
    EXPECT_TRUE(disk_page.verify_checksum());
    auto rec = disk_page.read_record(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->first, "frame_key");
    EXPECT_EQ(rec->second, "frame_val");
}

// ── Test 4: KVStore with Direct I/O ────────────────────────────────
TEST_F(DirectIOTest, KVStoreDirectIO) {
    {
        KVStore store(test_file, 64, IOMode::DIRECT);
        EXPECT_EQ(store.io_mode(), IOMode::DIRECT);

        for (int i = 0; i < 50; ++i) {
            store.put("k_" + std::to_string(i), "v_" + std::to_string(i * 3));
        }
        store.flush();
        EXPECT_EQ(store.size(), 50u);
    }

    // Reopen in direct mode and verify persistence
    {
        KVStore reopened(test_file, 64, IOMode::DIRECT);
        EXPECT_EQ(reopened.size(), 50u);

        for (int i = 0; i < 50; ++i) {
            auto val = reopened.get("k_" + std::to_string(i));
            ASSERT_TRUE(val.has_value());
            EXPECT_EQ(*val, "v_" + std::to_string(i * 3));
        }

        // Delete test
        EXPECT_TRUE(reopened.remove("k_10"));
        EXPECT_FALSE(reopened.get("k_10").has_value());
        EXPECT_EQ(reopened.size(), 49u);
    }
}

// ── Test 5: Misaligned Buffer Throws Clear Diagnosable EINVAL ───────
TEST_F(DirectIOTest, MisalignedBufferThrowsEINVAL) {
    FileManager fm(test_file, IOMode::DIRECT);

    // Allocate 8192 bytes aligned to 4096
    alignas(4096) std::byte aligned_buf[PAGE_SIZE * 2];

    // Pointer offset by 1 byte -> INTENTIONALLY MISALIGNED!
    std::byte* misaligned_ptr = aligned_buf + 1;

    // Passing misaligned buffer directly to pwrite on an O_DIRECT fd
    // Linux kernel MUST return -1 with errno == EINVAL
    ssize_t ret = ::pwrite(fm.fd(), misaligned_ptr, PAGE_SIZE, 0);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EINVAL) << "Unaligned buffer on O_DIRECT fd must produce EINVAL";

    // Verify FileManager::write_raw_aligned throws our custom diagnostic error
    try {
        fm.write_raw_aligned(0, misaligned_ptr);
        FAIL() << "Expected std::runtime_error containing EINVAL";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("O_DIRECT alignment error (EINVAL)"), std::string::npos)
            << "Error message must clearly identify O_DIRECT EINVAL alignment failure: " << msg;
    }
}

// ── Test 6: Full Correctness Equivalence (Buffered vs Direct) ──────
TEST_F(DirectIOTest, EquivalenceBufferedVsDirect) {
    std::string direct_db = test_file + "_direct.db";
    std::string buffered_db = test_file + "_buffered.db";

    KVStore direct_store(direct_db, 64, IOMode::DIRECT);
    KVStore buffered_store(buffered_db, 64, IOMode::BUFFERED);

    for (int i = 0; i < 200; ++i) {
        std::string key = "item_" + std::to_string(i);
        std::string val = "data_" + std::to_string(i * 7);

        direct_store.put(key, val);
        buffered_store.put(key, val);
    }

    direct_store.flush();
    buffered_store.flush();

    EXPECT_EQ(direct_store.size(), buffered_store.size());

    for (int i = 0; i < 200; ++i) {
        std::string key = "item_" + std::to_string(i);
        auto val_d = direct_store.get(key);
        auto val_b = buffered_store.get(key);

        ASSERT_TRUE(val_d.has_value());
        ASSERT_TRUE(val_b.has_value());
        EXPECT_EQ(*val_d, *val_b);
    }

    std::filesystem::remove(direct_db); std::filesystem::remove(direct_db + ".wal");
    std::filesystem::remove(buffered_db); std::filesystem::remove(buffered_db + ".wal");
}
