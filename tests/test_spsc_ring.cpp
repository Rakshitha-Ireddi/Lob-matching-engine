// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "lob/spsc_ring.hpp"

using namespace lob;

TEST(SpscRing, CapacityRoundsUpToPowerOfTwo) {
    SpscRing<int> r(100);
    EXPECT_EQ(r.capacity(), 128u);
}

TEST(SpscRing, FifoOrderAndEmptyFull) {
    SpscRing<int> r(4);  // holds capacity-1 = 3 usable
    int v = 0;
    EXPECT_FALSE(r.try_pop(v));
    EXPECT_TRUE(r.try_push(1));
    EXPECT_TRUE(r.try_push(2));
    EXPECT_TRUE(r.try_push(3));
    EXPECT_FALSE(r.try_push(4));  // full
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(r.try_push(4));
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 4);
    EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, ThreadedStreamPreservesEveryValueInOrder) {
    constexpr std::uint64_t N = 2'000'000;
    SpscRing<std::uint64_t> r(4096);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            while (!r.try_push(i)) {
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t got = 0;
    bool ordered = true;
    while (got < N) {
        std::uint64_t v;
        if (r.try_pop(v)) {
            if (v != expected) ordered = false;
            ++expected;
            ++got;
        }
    }
    producer.join();
    EXPECT_TRUE(ordered);
    EXPECT_EQ(got, N);
}
