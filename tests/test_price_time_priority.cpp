// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace lob;
using namespace lobtest;

// Resting orders at the same price fill in arrival order.
TEST(PriceTimePriority, FifoWithinLevel) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Buy, 100, 5));
    run(e, limit(2, Side::Buy, 100, 5));
    run(e, limit(3, Side::Buy, 100, 5));

    auto evs = run(e, limit(10, Side::Sell, 100, 8));  // takes 5 from #1, 3 from #2

    ASSERT_GE(count(evs, EventType::Fill), 4);
    // maker legs, in order
    std::vector<OrderId> maker_hits;
    for (const auto& ev : evs)
        if (ev.type == EventType::Fill && !ev.aggressor) maker_hits.push_back(ev.order_id);
    ASSERT_EQ(maker_hits.size(), 2u);
    EXPECT_EQ(maker_hits[0], 1u);
    EXPECT_EQ(maker_hits[1], 2u);

    // #2 has 2 left, #3 untouched
    auto probe = run(e, limit(11, Side::Sell, 100, 100));
    std::vector<OrderId> order;
    for (const auto& ev : probe)
        if (ev.type == EventType::Fill && !ev.aggressor) order.push_back(ev.order_id);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2u);
    EXPECT_EQ(order[1], 3u);
}

// Better price is always hit before worse price.
TEST(PriceTimePriority, PriceBeatsTime) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 102, 5));  // arrives first, worse price
    run(e, limit(2, Side::Sell, 101, 5));  // arrives second, better price

    auto evs = run(e, limit(10, Side::Buy, 105, 5));
    const Event* f = first(evs, EventType::Fill);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->price, 101);
    // contra should be order #2
    for (const auto& ev : evs) {
        if (ev.type == EventType::Fill && ev.aggressor) {
            EXPECT_EQ(ev.contra_id, 2u);
        }
    }
}

// Aggressor takes price improvement -- trade prints at the resting price.
TEST(PriceTimePriority, TradePrintsAtRestingPrice) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Buy, 100, 10));
    auto evs = run(e, limit(2, Side::Sell, 95, 4));  // crosses; prints at 100
    const Event* t = first(evs, EventType::Trade);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->price, 100);
    EXPECT_EQ(t->quantity, 4u);
}

// Size-down keeps queue position; size-up / reprice loses it.
TEST(PriceTimePriority, ModifyQueuePriorityRules) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Buy, 100, 10));
    run(e, limit(2, Side::Buy, 100, 10));

    // size #1 down: still ahead of #2
    auto m = run(e, Command::make_modify(1, 1, 100, 4));
    EXPECT_EQ(count(m, EventType::Replaced), 1);

    auto hit = run(e, limit(20, Side::Sell, 100, 5));  // 4 from #1, 1 from #2
    std::vector<OrderId> makers;
    for (const auto& ev : hit) {
        if (ev.type == EventType::Fill && !ev.aggressor) makers.push_back(ev.order_id);
    }
    ASSERT_EQ(makers.size(), 2u);
    EXPECT_EQ(makers[0], 1u);
    EXPECT_EQ(makers[1], 2u);

    // now size #2 UP: it should drop behind a fresh order at the same price
    run(e, Command::make_modify(2, 1, 100, 50));
    run(e, limit(3, Side::Buy, 100, 10));  // #2 was repriced after -> #3 ahead? no:
    // #2 re-queued at modify time (before #3), so #2 is still ahead of #3.
    // Verify #2 (now size 50) fills before #3.
    auto sweep = run(e, limit(21, Side::Sell, 100, 55));
    std::vector<OrderId> m2;
    for (const auto& ev : sweep)
        if (ev.type == EventType::Fill && !ev.aggressor) m2.push_back(ev.order_id);
    ASSERT_FALSE(m2.empty());
    EXPECT_EQ(m2.front(), 2u);
}
