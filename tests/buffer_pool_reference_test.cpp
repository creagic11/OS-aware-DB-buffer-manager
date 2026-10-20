/**
 * @file buffer_pool_reference_test.cpp
 * @brief Phase 2/3 exit criterion: hit/miss/eviction counts match an
 *        independent reference model across dozens of scripted sequences,
 *        including sequences that force eviction and pin pages.
 *
 * The reference model is a deliberately naive re-implementation of the
 * specified policy (free frames handed out 0,1,2,...; CLOCK per spec pseudo-
 * code, or LRU by last access) that tracks only residency, pins and bits.
 */

#include <gtest/gtest.h>

#include "bufferscope/buffer_pool.hpp"

#include <filesystem>
#include <optional>
#include <random>
#include <unistd.h>
#include <vector>

using namespace bufferscope;

namespace {

struct ModelFrame {
    bool valid = false;
    uint32_t pid = 0;
    uint32_t pin = 0;
    bool ref = false;
    uint64_t last = 0;
};

class ReferenceModel {
public:
    ReferenceModel(size_t n, ReplacerType t) : frames_(n), type_(t) {}

    // Returns true if the fetch succeeds (model prediction of non-null).
    bool fetch(uint32_t pid) {
        ++clock_;
        for (auto& f : frames_) {
            if (f.valid && f.pid == pid) {
                ++hits;
                f.pin++;
                f.ref = true;
                f.last = clock_;
                return true;
            }
        }
        ++misses;
        std::optional<size_t> slot;
        for (size_t i = 0; i < frames_.size(); ++i) {
            if (!frames_[i].valid) { slot = i; break; }
        }
        if (!slot) slot = victim();
        if (!slot) return false;
        if (frames_[*slot].valid) ++evictions;
        frames_[*slot] = ModelFrame{true, pid, 1, true, clock_};
        return true;
    }

    void unpin(uint32_t pid) {
        for (auto& f : frames_)
            if (f.valid && f.pid == pid && f.pin > 0) { f.pin--; return; }
    }

    bool resident(uint32_t pid) const {
        for (auto& f : frames_)
            if (f.valid && f.pid == pid) return true;
        return false;
    }

    uint64_t hits = 0, misses = 0, evictions = 0;

private:
    std::optional<size_t> victim() {
        const size_t n = frames_.size();
        if (type_ == ReplacerType::LRU) {
            std::optional<size_t> best;
            for (size_t i = 0; i < n; ++i)
                if (frames_[i].pin == 0 && (!best || frames_[i].last < frames_[*best].last)) best = i;
            return best;
        }
        for (size_t step = 0; step < 2 * n; ++step) {
            size_t i = hand_;
            hand_ = (hand_ + 1) % n;
            if (frames_[i].pin > 0) continue;
            if (frames_[i].ref) { frames_[i].ref = false; continue; }
            return i;
        }
        return std::nullopt;
    }

    std::vector<ModelFrame> frames_;
    ReplacerType type_;
    size_t hand_ = 0;
    uint64_t clock_ = 0;
};

struct Param {
    ReplacerType type;
};

class BufferPoolReferenceTest : public ::testing::TestWithParam<Param> {
protected:
    std::string path;
    void SetUp() override {
        path = "/tmp/bufferscope_refmodel_" + std::to_string(::getpid()) + ".db";
        std::filesystem::remove(path);
    }
    void TearDown() override { std::filesystem::remove(path); }
};

}  // namespace

TEST_P(BufferPoolReferenceTest, DozensOfScriptedSequencesMatchModel) {
    const ReplacerType type = GetParam().type;
    std::mt19937_64 rng(2024);
    int sequences = 0;

    // 6 pool sizes x 4 access shapes x 3 seeds = 72 scripted sequences.
    for (size_t pool : {1u, 2u, 3u, 4u, 8u, 16u}) {
        for (int shape = 0; shape < 4; ++shape) {
            for (int rep = 0; rep < 3; ++rep) {
                std::filesystem::remove(path);
                FileManager fm(path);
                BufferPoolManager bpm(pool, fm, type);
                ReferenceModel model(pool, type);
                std::vector<uint32_t> held;  // pages currently pinned by the script

                const uint32_t range = static_cast<uint32_t>(pool * (shape == 0 ? 1 : 3));
                for (int step = 0; step < 400; ++step) {
                    uint32_t pid;
                    switch (shape) {
                        case 0:  // working set fits in the pool
                        case 1:  // uniform over 3x pool
                            pid = static_cast<uint32_t>(rng() % range);
                            break;
                        case 2:  // sequential scan over 3x pool
                            pid = static_cast<uint32_t>(step % range);
                            break;
                        default:  // hot/cold: 80% of accesses to 2 pages
                            pid = (rng() % 10 < 8) ? static_cast<uint32_t>(rng() % 2)
                                                   : static_cast<uint32_t>(2 + rng() % range);
                    }

                    const bool expect_ok = model.fetch(pid);
                    Frame* f = bpm.fetch_page(pid);
                    ASSERT_EQ(f != nullptr, expect_ok)
                        << "pool=" << pool << " shape=" << shape << " step=" << step;
                    if (f) {
                        ASSERT_EQ(f->page_id, pid);
                        // Hold roughly 1 in 8 pages pinned for a while.
                        if (rng() % 8 == 0 && held.size() + 1 < pool) {
                            held.push_back(pid);
                        } else {
                            bpm.unpin_page(pid, rng() % 4 == 0);
                            model.unpin(pid);
                        }
                    }
                    if (!held.empty() && rng() % 5 == 0) {
                        uint32_t h = held.back();
                        held.pop_back();
                        bpm.unpin_page(h, false);
                        model.unpin(h);
                    }
                    ASSERT_EQ(bpm.is_resident(pid), model.resident(pid));
                }
                EXPECT_EQ(bpm.metrics().get_hits(), model.hits);
                EXPECT_EQ(bpm.metrics().get_misses(), model.misses);
                EXPECT_EQ(bpm.metrics().get_evictions(), model.evictions);
                for (uint32_t h : held) bpm.unpin_page(h, false);
                ++sequences;
            }
        }
    }
    EXPECT_EQ(sequences, 72);
}

TEST_P(BufferPoolReferenceTest, AllPinnedReturnsNullAndRecovers) {
    FileManager fm(path);
    BufferPoolManager bpm(3, fm, GetParam().type);
    for (uint32_t p = 0; p < 3; ++p) ASSERT_NE(bpm.fetch_page(p), nullptr);
    EXPECT_EQ(bpm.fetch_page(99), nullptr);
    EXPECT_EQ(bpm.pinned_frames(), 3u);
    for (uint32_t p = 0; p < 3; ++p) EXPECT_TRUE(bpm.is_resident(p));  // nothing evicted
    bpm.unpin_page(1, false);
    Frame* f = bpm.fetch_page(99);
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(bpm.is_resident(1));
}

INSTANTIATE_TEST_SUITE_P(Policies, BufferPoolReferenceTest,
                         ::testing::Values(Param{ReplacerType::CLOCK}, Param{ReplacerType::LRU}),
                         [](const auto& info) { return std::string(replacer_name(info.param.type)); });
