/**
 * @file io_mode_correctness_test.cpp
 * @brief Phase 5 requirement: run the Phase 0-3 correctness suite against BOTH
 *        the buffered and the O_DIRECT path and require identical results.
 *
 * Also contains regression tests for defects found in the Phase 0-5 review:
 *  - oversized records were silently truncated through uint16_t arithmetic;
 *  - a failed page load leaked its frame (pool shrank permanently);
 *  - byte/flush/eviction metrics were declared but never recorded;
 *  - corrupted pages loaded through the pool were not checksum-verified.
 */

#include <gtest/gtest.h>

#include "bufferscope/buffer_pool.hpp"
#include "bufferscope/kv_store.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <unistd.h>

using namespace bufferscope;

namespace {

class IOModeTest : public ::testing::TestWithParam<IOMode> {
protected:
    std::string path;
    void SetUp() override {
        path = "/tmp/bufferscope_iomode_" + std::to_string(::getpid()) + "_" +
               io_mode_name(GetParam()) + ".db";
        std::filesystem::remove(path);
    }
    void TearDown() override { std::filesystem::remove(path); }

    void corrupt_byte(off_t off) {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekg(off);
        char c = 0;
        f.get(c);
        f.seekp(off);
        f.put(static_cast<char>(~c));
    }
};

// Same 10,000-op randomized oracle test as Phase 1, with a deliberately tiny
// pool (8 frames) so the run exercises eviction + dirty write-back on every path.
std::map<std::string, std::string> run_oracle(KVStore& store, uint64_t seed) {
    std::map<std::string, std::string> ref;
    std::mt19937_64 rng(seed);
    for (int i = 0; i < 10000; ++i) {
        const int op = static_cast<int>(rng() % 10);
        const std::string key = "key_" + std::to_string(rng() % 700);
        if (op <= 4) {
            std::string v(5 + rng() % 120, static_cast<char>('a' + rng() % 26));
            store.put(key, v);
            ref[key] = v;
        } else if (op <= 7) {
            auto got = store.get(key);
            auto it = ref.find(key);
            if (it == ref.end()) {
                EXPECT_FALSE(got.has_value()) << key;
            } else {
                EXPECT_EQ(got.value_or("<missing>"), it->second) << key;
            }
        } else {
            EXPECT_EQ(store.remove(key), ref.erase(key) == 1) << key;
        }
    }
    return ref;
}

}  // namespace

TEST_P(IOModeTest, RandomizedOracleWithEvictionAndRestart) {
    std::map<std::string, std::string> ref;
    {
        KVStore store(path, 8, GetParam());
        ref = run_oracle(store, 7);
        EXPECT_EQ(store.size(), ref.size());
        EXPECT_GT(store.buffer_pool().metrics().get_evictions(), 0u);
        EXPECT_GT(store.buffer_pool().metrics().get_dirty_evictions(), 0u);
    }
    KVStore reopened(path, 8, GetParam());
    ASSERT_EQ(reopened.size(), ref.size());
    for (const auto& [k, v] : ref) EXPECT_EQ(reopened.get(k).value_or("<missing>"), v) << k;
}

TEST_P(IOModeTest, CorruptedPageDetectedOnLoad) {
    {
        KVStore store(path, 4, GetParam());
        for (int i = 0; i < 300; ++i) store.put("k" + std::to_string(i), std::string(40, 'x'));
    }
    ASSERT_GE(std::filesystem::file_size(path), 3u * PAGE_SIZE);
    corrupt_byte(static_cast<off_t>(PAGE_SIZE) + 2000);  // inside page 1
    EXPECT_THROW({ KVStore s(path, 4, GetParam()); }, ChecksumMismatchError);
}

TEST_P(IOModeTest, FailedLoadDoesNotLeakFrames) {
    {
        FileManager fm(path, GetParam());
        for (uint32_t p = 0; p < 4; ++p) {
            Page pg(p);
            pg.insert_record("a", "b");
            fm.write_page(pg);
        }
        fm.flush();
    }
    corrupt_byte(static_cast<off_t>(2 * PAGE_SIZE) + 100);
    FileManager fm(path, GetParam());
    BufferPoolManager bpm(2, fm);
    for (int attempt = 0; attempt < 5; ++attempt) {
        EXPECT_THROW(bpm.fetch_page(2), ChecksumMismatchError);
    }
    EXPECT_FALSE(bpm.is_resident(2));
    // Both frames must still be usable.
    ASSERT_NE(bpm.fetch_page(0), nullptr);
    ASSERT_NE(bpm.fetch_page(1), nullptr);
}

TEST_P(IOModeTest, MetricsCountBytesFlushesAndEvictions) {
    FileManager fm(path, GetParam());
    BufferPoolManager bpm(2, fm);
    for (uint32_t p = 0; p < 6; ++p) {
        Frame* f = bpm.fetch_page(p);
        ASSERT_NE(f, nullptr);
        bpm.unpin_page(p, true);
    }
    bpm.flush_all_pages();
    // 6 dirty pages: 4 written back by eviction, 2 by the final flush.
    EXPECT_EQ(bpm.metrics().get_evictions(), 4u);
    EXPECT_EQ(bpm.metrics().get_dirty_evictions(), 4u);
    EXPECT_EQ(bpm.metrics().get_page_flushes(), 6u);
    EXPECT_EQ(bpm.metrics().get_bytes_written(), 6u * PAGE_SIZE);
    EXPECT_EQ(fm.stats().bytes_written.load(), 6u * PAGE_SIZE);
    EXPECT_GE(fm.stats().syncs.load(), 1u);

    // Re-reading evicted pages is counted identically at both layers.
    for (uint32_t p = 0; p < 4; ++p) {
        ASSERT_NE(bpm.fetch_page(p), nullptr);
        bpm.unpin_page(p, false);
    }
    EXPECT_EQ(bpm.metrics().get_bytes_read(), fm.stats().bytes_read.load());
    EXPECT_GT(bpm.metrics().get_bytes_read(), 0u);
}

INSTANTIATE_TEST_SUITE_P(BothPaths, IOModeTest,
                         ::testing::Values(IOMode::BUFFERED, IOMode::DIRECT),
                         [](const auto& info) { return std::string(io_mode_name(info.param)); });

// ── Mode-independent regressions ────────────────────────────────────

TEST(ReviewRegression, OversizedRecordRejectedNotTruncated) {
    Page p(0u);
    // 65,540 bytes would wrap to 4 in uint16_t arithmetic and "fit".
    EXPECT_FALSE(p.insert_record(std::string(65536, 'k'), "v"));
    EXPECT_FALSE(p.insert_record("k", std::string(MAX_KV_BYTES, 'v')));
    EXPECT_TRUE(p.insert_record("k", std::string(MAX_KV_BYTES - 1, 'v')));
    EXPECT_EQ(p.free_space(), 0u);

    std::string path = "/tmp/bufferscope_oversize_" + std::to_string(::getpid()) + ".db";
    std::filesystem::remove(path);
    {
        KVStore s(path);
        EXPECT_THROW(s.put("k", std::string(70000, 'v')), std::invalid_argument);
        EXPECT_NO_THROW(s.put("k", std::string(MAX_KV_BYTES - 1, 'v')));
        EXPECT_EQ(s.get("k")->size(), MAX_KV_BYTES - 1);
    }
    std::filesystem::remove(path);
}

TEST(ReviewRegression, MissingPageIsDistinctFromChecksumMismatch) {
    std::string path = "/tmp/bufferscope_missing_" + std::to_string(::getpid()) + ".db";
    std::filesystem::remove(path);
    FileManager fm(path);
    Page p(0u);
    fm.write_page(p);
    EXPECT_THROW(fm.read_page(5), PageNotFoundError);
    EXPECT_NO_THROW(fm.read_page_verified(0));
    std::filesystem::remove(path);
}

TEST(ReviewRegression, DirectIOMisalignedOffsetIsEINVAL) {
    std::string path = "/tmp/bufferscope_misoff_" + std::to_string(::getpid()) + ".db";
    std::filesystem::remove(path);
    FileManager fm(path, IOMode::DIRECT);
    alignas(4096) std::byte buf[PAGE_SIZE] = {};
    // Aligned buffer, aligned length, misaligned file offset.
    errno = 0;
    EXPECT_EQ(::pwrite(fm.fd(), buf, PAGE_SIZE, 1), -1);
    EXPECT_EQ(errno, EINVAL);
    // Aligned buffer, misaligned length.
    errno = 0;
    EXPECT_EQ(::pwrite(fm.fd(), buf, 100, 0), -1);
    EXPECT_EQ(errno, EINVAL);
    // Our API never issues those; a misaligned *buffer* surfaces as the typed error.
    EXPECT_THROW(fm.write_raw_aligned(0, buf + 8), DirectIOAlignmentError);
    std::filesystem::remove(path);
}
