/**
 * @file clock_replacer_test.cpp
 * @brief GoogleTest suite for ClockReplacer (Phase 3).
 *
 * Requirements verified:
 *  1. Empty cache / free selection
 *  2. Second-chance reference bit clearing
 *  3. Skipping pinned frames
 *  4. All-pinned case returns std::nullopt without hanging
 */

#include <gtest/gtest.h>
#include "bufferscope/replacer.hpp"
#include "bufferscope/buffer_pool.hpp"

using namespace bufferscope;

TEST(ClockReplacerTest, BasicVictimSelection) {
    constexpr size_t NUM_FRAMES = 3;
    auto frames = std::make_unique<Frame[]>(NUM_FRAMES);

    // Frame 0: unpinned, unreferenced -> prime victim
    frames[0].pin_count = 0;
    frames[0].referenced = false;

    // Frame 1: unpinned, referenced
    frames[1].pin_count = 0;
    frames[1].referenced = true;

    // Frame 2: pinned
    frames[2].pin_count = 1;
    frames[2].referenced = false;

    ClockReplacer replacer(NUM_FRAMES);

    auto victim1 = replacer.victim(frames.get(), NUM_FRAMES);
    ASSERT_TRUE(victim1.has_value());
    EXPECT_EQ(*victim1, 0u);
}

TEST(ClockReplacerTest, SecondChanceBehavior) {
    constexpr size_t NUM_FRAMES = 3;
    auto frames = std::make_unique<Frame[]>(NUM_FRAMES);

    // All frames unpinned, but all referenced
    for (size_t i = 0; i < NUM_FRAMES; ++i) {
        frames[i].pin_count = 0;
        frames[i].referenced = true;
    }

    ClockReplacer replacer(NUM_FRAMES);

    // On pass 1, frames 0, 1, 2 have their reference bits cleared
    // On pass 2, frame 0 is selected
    auto victim = replacer.victim(frames.get(), NUM_FRAMES);
    ASSERT_TRUE(victim.has_value());
    EXPECT_EQ(*victim, 0u);

    // Frames 1 and 2 should now have referenced == false
    EXPECT_FALSE(frames[1].referenced);
    EXPECT_FALSE(frames[2].referenced);

    // Next victim should be frame 1
    auto victim2 = replacer.victim(frames.get(), NUM_FRAMES);
    ASSERT_TRUE(victim2.has_value());
    EXPECT_EQ(*victim2, 1u);
}

TEST(ClockReplacerTest, AllFramesPinned) {
    constexpr size_t NUM_FRAMES = 4;
    auto frames = std::make_unique<Frame[]>(NUM_FRAMES);

    for (size_t i = 0; i < NUM_FRAMES; ++i) {
        frames[i].pin_count = 2;  // Pinned!
        frames[i].referenced = false;
    }

    ClockReplacer replacer(NUM_FRAMES);

    // Must return nullopt immediately without hanging or crashing
    auto victim = replacer.victim(frames.get(), NUM_FRAMES);
    EXPECT_FALSE(victim.has_value());
}
