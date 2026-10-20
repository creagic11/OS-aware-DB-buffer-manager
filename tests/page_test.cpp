/**
 * @file page_test.cpp
 * @brief GoogleTest cases for the Page abstraction (Phase 0).
 *
 * Tests:
 *  1. Basic construction and serialization
 *  2. Deterministic serialization (same page => same bytes)
 *  3. Record insert and read-back
 *  4. Checksum computation and verification
 *  5. Corrupted page detection
 *  6. Page full detection
 *  7. Record deletion
 */

#include <gtest/gtest.h>
#include "bufferscope/page.hpp"
#include <cstring>

using namespace bufferscope;

// ── Test: basic construction ───────────────────────────────────────
TEST(PageTest, BasicConstruction) {
    Page page(42);
    EXPECT_EQ(page.page_id(), 42u);
    EXPECT_EQ(page.record_count(), 0u);
    EXPECT_EQ(page.lsn(), 0u);
}

// ── Test: deterministic serialization ──────────────────────────────
TEST(PageTest, DeterministicSerialization) {
    Page page1(7);
    page1.insert_record("key1", "value1");
    page1.update_checksum();

    Page page2(7);
    page2.insert_record("key1", "value1");
    page2.update_checksum();

    auto buf1 = page1.serialize();
    auto buf2 = page2.serialize();

    EXPECT_EQ(buf1, buf2) << "Same page content must produce identical bytes";
}

// ── Test: record insert and read ───────────────────────────────────
TEST(PageTest, InsertAndRead) {
    Page page(0u);
    ASSERT_TRUE(page.insert_record("hello", "world"));
    ASSERT_TRUE(page.insert_record("foo", "bar"));

    EXPECT_EQ(page.record_count(), 2u);

    auto rec0 = page.read_record(0);
    ASSERT_TRUE(rec0.has_value());
    EXPECT_EQ(rec0->first, "hello");
    EXPECT_EQ(rec0->second, "world");

    auto rec1 = page.read_record(1);
    ASSERT_TRUE(rec1.has_value());
    EXPECT_EQ(rec1->first, "foo");
    EXPECT_EQ(rec1->second, "bar");

    // Out-of-bounds slot
    auto rec2 = page.read_record(2);
    EXPECT_FALSE(rec2.has_value());
}

// ── Test: checksum computation and verification ────────────────────
TEST(PageTest, ChecksumVerification) {
    Page page(10);
    page.insert_record("test_key", "test_value");
    page.update_checksum();

    // Verify passes
    EXPECT_TRUE(page.verify_checksum());

    // Serialize, deserialize, verify still passes
    auto buf = page.serialize();
    Page deserialized(buf.data());
    EXPECT_TRUE(deserialized.verify_checksum());
}

// ── Test: corrupted page detection ─────────────────────────────────
TEST(PageTest, CorruptedPageDetection) {
    Page page(5);
    page.insert_record("important", "data");
    page.update_checksum();

    auto buf = page.serialize();

    // Flip a byte in the payload area
    buf[100] = std::byte{static_cast<uint8_t>(~static_cast<uint8_t>(buf[100]))};

    Page corrupted(buf.data());
    EXPECT_FALSE(corrupted.verify_checksum())
        << "Corrupted page must fail checksum verification";
}

// ── Test: page full detection ──────────────────────────────────────
TEST(PageTest, PageFullDetection) {
    Page page(0u);
    int inserted = 0;

    // Keep inserting until the page is full
    // With PAGE_SIZE=4096, header=32, each record = 4 + key + value + 4 (slot)
    // Small records: key="kXXX", val="vXXX" => ~16 bytes per record + 4 slot
    // Should fit ~200 small records
    for (int i = 0; i < 500; ++i) {
        std::string key = "k" + std::to_string(i);
        std::string val = "v" + std::to_string(i);
        if (!page.insert_record(key, val)) {
            break;
        }
        inserted++;
    }

    EXPECT_GT(inserted, 0) << "Must be able to insert at least one record";
    EXPECT_LT(inserted, 500) << "Page should have filled up before 500 records";

    // The last insert should have failed, meaning page is full
    EXPECT_FALSE(page.insert_record("overflow", "data"));
}

// ── Test: record deletion ──────────────────────────────────────────
TEST(PageTest, RecordDeletion) {
    Page page(0u);
    page.insert_record("key1", "val1");
    page.insert_record("key2", "val2");
    page.insert_record("key3", "val3");

    EXPECT_EQ(page.record_count(), 3u);

    // Delete middle record
    EXPECT_TRUE(page.delete_record(1));

    // Reading deleted slot should return nullopt
    auto deleted = page.read_record(1);
    EXPECT_FALSE(deleted.has_value());

    // Other records still intact
    auto rec0 = page.read_record(0);
    ASSERT_TRUE(rec0.has_value());
    EXPECT_EQ(rec0->first, "key1");

    auto rec2 = page.read_record(2);
    ASSERT_TRUE(rec2.has_value());
    EXPECT_EQ(rec2->first, "key3");

    // Double delete should fail
    EXPECT_FALSE(page.delete_record(1));
}

// ── Test: CRC32 basic ──────────────────────────────────────────────
TEST(PageTest, CRC32Basic) {
    const char* data = "Hello, BufferScope!";
    auto bytes = reinterpret_cast<const std::byte*>(data);
    uint32_t checksum = crc32(bytes, strlen(data));

    // Same data should produce same checksum
    uint32_t checksum2 = crc32(bytes, strlen(data));
    EXPECT_EQ(checksum, checksum2);

    // Different data should (almost certainly) produce different checksum
    const char* data2 = "Hello, BufferScope?";
    auto bytes2 = reinterpret_cast<const std::byte*>(data2);
    uint32_t checksum3 = crc32(bytes2, strlen(data2));
    EXPECT_NE(checksum, checksum3);
}

// ── Test: serialize-deserialize roundtrip ──────────────────────────
TEST(PageTest, SerializeDeserializeRoundtrip) {
    Page original(99);
    original.insert_record("name", "BufferScope");
    original.insert_record("version", "0.1.0");
    original.set_lsn(42);
    original.update_checksum();

    auto buf = original.serialize();
    Page restored(buf.data());

    EXPECT_EQ(restored.page_id(), 99u);
    EXPECT_EQ(restored.lsn(), 42u);
    EXPECT_EQ(restored.record_count(), 2u);
    EXPECT_TRUE(restored.verify_checksum());

    auto rec0 = restored.read_record(0);
    ASSERT_TRUE(rec0.has_value());
    EXPECT_EQ(rec0->first, "name");
    EXPECT_EQ(rec0->second, "BufferScope");

    auto rec1 = restored.read_record(1);
    ASSERT_TRUE(rec1.has_value());
    EXPECT_EQ(rec1->first, "version");
    EXPECT_EQ(rec1->second, "0.1.0");
}

// ── Test: empty page serialization ─────────────────────────────────
TEST(PageTest, EmptyPageSerialization) {
    Page page(0u);
    page.update_checksum();

    auto buf = page.serialize();
    Page restored(buf.data());

    EXPECT_EQ(restored.page_id(), 0u);
    EXPECT_EQ(restored.record_count(), 0u);
    EXPECT_TRUE(restored.verify_checksum());
}
