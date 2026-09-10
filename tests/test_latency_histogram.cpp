// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "lob/latency_histogram.hpp"

using namespace lob;

TEST(LatencyHistogram, ExactSmallValues) {
    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) h.record(0);
    for (int i = 0; i < 100; ++i) h.record(1);
    for (int i = 0; i < 100; ++i) h.record(1500);
    EXPECT_EQ(h.count(), 300u);
    EXPECT_EQ(h.min(), 0u);
    EXPECT_EQ(h.max(), 1500u);
}

TEST(LatencyHistogram, PercentilesWithinRelativeError) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100000; ++v) h.record(v);  // uniform 1..100k ns

    auto near = [](std::uint64_t got, double want) {
        return std::fabs(static_cast<double>(got) - want) / want < 0.02;
    };
    EXPECT_TRUE(near(h.percentile(50), 50000)) << h.percentile(50);
    EXPECT_TRUE(near(h.percentile(90), 90000)) << h.percentile(90);
    EXPECT_TRUE(near(h.percentile(99), 99000)) << h.percentile(99);
    EXPECT_GE(h.percentile(100), 99999u);
}

TEST(LatencyHistogram, MergeCombinesCounts) {
    LatencyHistogram a, b;
    for (int i = 0; i < 1000; ++i) a.record(500);
    for (int i = 0; i < 1000; ++i) b.record(5000);
    a.merge(b);
    EXPECT_EQ(a.count(), 2000u);
    EXPECT_EQ(a.max(), 5000u);
    EXPECT_LE(a.percentile(25), 600u);
    EXPECT_GE(a.percentile(75), 4900u);
}

TEST(LatencyHistogram, BinsCoverAllSamplesMonotonic) {
    LatencyHistogram h;
    for (std::uint64_t v : {5ULL, 50ULL, 500ULL, 5000ULL, 50000ULL, 500000ULL})
        for (int i = 0; i < 10; ++i) h.record(v);

    std::uint64_t total = 0;
    std::uint64_t prev_hi = 0;
    for (const auto& b : h.bins()) {
        EXPECT_LE(b.lo, b.hi);
        EXPECT_GE(b.lo, prev_hi);
        prev_hi = b.hi;
        total += b.count;
    }
    EXPECT_EQ(total, h.count());
}
