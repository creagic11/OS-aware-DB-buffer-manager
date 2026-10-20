/**
 * @file kv_correctness_test.cpp
 * @brief GoogleTest suite for KVStore (Phase 1).
 *
 * Requirements verified:
 *  1. Basic PUT, GET, DELETE operations
 *  2. Updating an existing key
 *  3. Non-existent key lookup returns nullopt
 *  4. Restart test (close, reopen, assert all keys retrievable)
 *  5. Checksum failure detection (intentionally corrupt a page and confirm error)
 *  6. 10,000 randomized operations against std::unordered_map reference model
 */

#include <gtest/gtest.h>
#include "bufferscope/kv_store.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
#include <vector>
#include <string>

using namespace bufferscope;

class KVStoreTest : public ::testing::Test {
protected:
    std::string test_file;

    void SetUp() override {
        test_file = "/tmp/bufferscope_kv_test_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }

    void TearDown() override {
        std::filesystem::remove(test_file); std::filesystem::remove(test_file + ".wal");
    }
};

// ── Test: Basic Operations ─────────────────────────────────────────
TEST_F(KVStoreTest, BasicPutGetDelete) {
    KVStore store(test_file);

    EXPECT_EQ(store.size(), 0u);
    EXPECT_TRUE(store.put("k1", "v1"));
    EXPECT_TRUE(store.put("k2", "v2"));
    EXPECT_EQ(store.size(), 2u);

    auto val1 = store.get("k1");
    ASSERT_TRUE(val1.has_value());
    EXPECT_EQ(*val1, "v1");

    auto val2 = store.get("k2");
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(*val2, "v2");

    // Non-existent key
    EXPECT_FALSE(store.get("k3").has_value());

    // Delete
    EXPECT_TRUE(store.remove("k1"));
    EXPECT_EQ(store.size(), 1u);
    EXPECT_FALSE(store.get("k1").has_value());

    // Double delete
    EXPECT_FALSE(store.remove("k1"));
}

// ── Test: Overwrite / Update Key ───────────────────────────────────
TEST_F(KVStoreTest, UpdateKey) {
    KVStore store(test_file);

    EXPECT_TRUE(store.put("alpha", "100"));
    EXPECT_EQ(*store.get("alpha"), "100");

    EXPECT_TRUE(store.put("alpha", "200"));
    EXPECT_EQ(*store.get("alpha"), "200");
    EXPECT_EQ(store.size(), 1u);

    EXPECT_TRUE(store.put("alpha", "final_value"));
    EXPECT_EQ(*store.get("alpha"), "final_value");
    EXPECT_EQ(store.size(), 1u);
}

// ── Test: Restart Test (Persistence across reopen) ─────────────────
TEST_F(KVStoreTest, RestartPersistence) {
    {
        KVStore store(test_file);
        for (int i = 0; i < 100; ++i) {
            store.put("user:" + std::to_string(i), "profile_" + std::to_string(i * 10));
        }
        store.flush();
        EXPECT_EQ(store.size(), 100u);
    }

    // Reopen from disk
    {
        KVStore reopened(test_file);
        EXPECT_EQ(reopened.size(), 100u);

        for (int i = 0; i < 100; ++i) {
            auto val = reopened.get("user:" + std::to_string(i));
            ASSERT_TRUE(val.has_value()) << "Missing key user:" << i;
            EXPECT_EQ(*val, "profile_" + std::to_string(i * 10));
        }
    }
}

// ── Test: Checksum Failure Detection ───────────────────────────────
TEST_F(KVStoreTest, ChecksumCorruptionDetection) {
    {
        KVStore store(test_file);
        store.put("critical_key", "critical_value");
        store.flush();
    }

    // Intentionally corrupt page 0 on disk
    {
        std::fstream f(test_file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        // Byte 120 is safely in the payload area
        f.seekp(120);
        char byte = 0;
        f.get(byte);
        f.seekp(120);
        byte = ~byte;
        f.put(byte);
        f.flush();
    }

    // Reopening the store should detect checksum corruption during index rebuild
    EXPECT_THROW({
        KVStore corrupt_store(test_file);
    }, std::runtime_error);
}

// ── Test: 10,000 Randomized Operations against Reference Model ─────
TEST_F(KVStoreTest, RandomizedTenThousandOperations) {
    KVStore store(test_file);
    std::unordered_map<std::string, std::string> reference;

    std::mt19937_64 rng(1337);  // Deterministic seed for reproducibility
    std::uniform_int_distribution<int> op_dist(0, 9);       // 0-4 PUT, 5-7 GET, 8-9 DELETE
    std::uniform_int_distribution<int> key_dist(0, 499);    // Key pool of 500 keys
    std::uniform_int_distribution<int> val_len_dist(5, 50);

    auto random_string = [&](int len) {
        static const char charset[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::string s;
        s.reserve(len);
        for (int i = 0; i < len; ++i) {
            s += charset[rng() % (sizeof(charset) - 1)];
        }
        return s;
    };

    constexpr int NUM_OPERATIONS = 10000;

    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        int op = op_dist(rng);
        std::string key = "key_" + std::to_string(key_dist(rng));

        if (op <= 4) {
            // PUT (50% probability)
            std::string val = random_string(val_len_dist(rng));
            store.put(key, val);
            reference[key] = val;
        } else if (op <= 7) {
            // GET (30% probability)
            auto val = store.get(key);
            auto ref_it = reference.find(key);
            if (ref_it == reference.end()) {
                EXPECT_FALSE(val.has_value()) << "Key " << key << " should not exist";
            } else {
                ASSERT_TRUE(val.has_value()) << "Key " << key << " expected: " << ref_it->second;
                EXPECT_EQ(*val, ref_it->second) << "Mismatch for key " << key;
            }
        } else {
            // DELETE (20% probability)
            bool deleted = store.remove(key);
            auto ref_it = reference.find(key);
            if (ref_it != reference.end()) {
                EXPECT_TRUE(deleted) << "Key " << key << " should have been deleted";
                reference.erase(ref_it);
            } else {
                EXPECT_FALSE(deleted) << "Key " << key << " did not exist, delete should fail";
            }
        }

        // Periodically check size
        if (i % 1000 == 0) {
            EXPECT_EQ(store.size(), reference.size()) << "Size mismatch at op " << i;
        }
    }

    store.flush();
    EXPECT_EQ(store.size(), reference.size());

    // Final verification of all remaining keys
    for (const auto& [k, v] : reference) {
        auto val = store.get(k);
        ASSERT_TRUE(val.has_value()) << "Final check: missing key " << k;
        EXPECT_EQ(*val, v) << "Final check: value mismatch for key " << k;
    }

    // Now test restart: close and reopen the database file
    {
        KVStore reopened(test_file);
        EXPECT_EQ(reopened.size(), reference.size());

        for (const auto& [k, v] : reference) {
            auto val = reopened.get(k);
            ASSERT_TRUE(val.has_value()) << "Post-restart: missing key " << k;
            EXPECT_EQ(*val, v) << "Post-restart: value mismatch for key " << k;
        }
    }
}
