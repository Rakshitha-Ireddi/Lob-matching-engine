// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace lob;
using namespace lobtest;

TEST(Engine, RestingLimitAcksWithFullLeaves) {
    MatchingEngine e(small_book());
    auto evs = run(e, limit(1, Side::Buy, 100, 10));
    ASSERT_EQ(count(evs, EventType::Accepted), 1);
    EXPECT_EQ(first(evs, EventType::Accepted)->leaves, 10u);
    EXPECT_EQ(count(evs, EventType::Fill), 0);
    EXPECT_EQ(e.resting_orders(), 1u);
    EXPECT_EQ(e.book().best_bid(), 100);
}

TEST(Engine, CrossingLimitFillsAndRestsRemainder) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 100, 4));
    auto evs = run(e, limit(2, Side::Buy, 100, 10));
    EXPECT_EQ(count(evs, EventType::Trade), 1);
    EXPECT_EQ(count(evs, EventType::Fill), 2);   // taker + maker
    EXPECT_EQ(e.book().best_bid(), 100);
    EXPECT_EQ(e.book().best_ask(), kNoPrice);
    EXPECT_EQ(e.book().best_level(Side::Buy)->total_qty, 6u);
}

TEST(Engine, MarketOrderSweepsThenCancelsRemainder) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 100, 3));
    run(e, limit(2, Side::Sell, 101, 3));
    auto evs = run(e, Command::make_new(9, 1, Side::Buy, OrderType::Market, kNoPrice, 10));
    EXPECT_EQ(count(evs, EventType::Trade), 2);
    ASSERT_EQ(count(evs, EventType::Canceled), 1);
    EXPECT_EQ(first(evs, EventType::Canceled)->quantity, 4u);  // 10 - 6 unfilled
    EXPECT_TRUE(e.book().empty(Side::Sell));
}

TEST(Engine, IocCancelsWhatItCannotFill) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 100, 2));
    auto evs = run(e, Command::make_new(9, 1, Side::Buy, OrderType::Ioc, 100, 10));
    EXPECT_EQ(count(evs, EventType::Trade), 1);
    EXPECT_EQ(count(evs, EventType::Canceled), 1);
    EXPECT_EQ(e.resting_orders(), 0u);
}

TEST(Engine, PostOnlyRejectsWhenMarketable) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 100, 5));
    auto cross = run(e, Command::make_new(2, 1, Side::Buy, OrderType::PostOnly, 100, 5));
    ASSERT_EQ(count(cross, EventType::Rejected), 1);
    EXPECT_EQ(first(cross, EventType::Rejected)->reason, RejectReason::PostOnlyWouldCross);

    auto rest = run(e, Command::make_new(3, 1, Side::Buy, OrderType::PostOnly, 99, 5));
    EXPECT_EQ(count(rest, EventType::Accepted), 1);
    EXPECT_EQ(count(rest, EventType::Rejected), 0);
}

TEST(Engine, FokAllOrNothing) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Sell, 100, 3));
    run(e, limit(2, Side::Sell, 101, 3));

    auto no = run(e, Command::make_new(9, 1, Side::Buy, OrderType::Fok, 101, 10));
    EXPECT_EQ(first(no, EventType::Rejected)->reason, RejectReason::FokUnfillable);
    EXPECT_EQ(count(no, EventType::Trade), 0);

    auto yes = run(e, Command::make_new(10, 1, Side::Buy, OrderType::Fok, 101, 6));
    EXPECT_EQ(count(yes, EventType::Trade), 2);
    EXPECT_TRUE(e.book().empty(Side::Sell));
}

TEST(Engine, Rejections) {
    MatchingEngine e(small_book(1, 1000));
    EXPECT_EQ(first(run(e, limit(1, Side::Buy, 100, 0)), EventType::Rejected)->reason,
              RejectReason::ZeroQuantity);
    EXPECT_EQ(first(run(e, limit(2, Side::Buy, 5000, 1)), EventType::Rejected)->reason,
              RejectReason::PriceOutOfBand);
    run(e, limit(3, Side::Buy, 100, 1));
    EXPECT_EQ(first(run(e, limit(3, Side::Buy, 100, 1)), EventType::Rejected)->reason,
              RejectReason::DuplicateOrderId);
    EXPECT_EQ(first(run(e, Command::make_cancel(999, 1)), EventType::Rejected)->reason,
              RejectReason::UnknownOrder);
}

TEST(Engine, CancelRemovesRestingOrder) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Buy, 100, 10));
    run(e, limit(2, Side::Buy, 100, 10));
    auto c = run(e, Command::make_cancel(1, 1));
    ASSERT_EQ(count(c, EventType::Canceled), 1);
    EXPECT_EQ(first(c, EventType::Canceled)->quantity, 10u);
    EXPECT_EQ(e.resting_orders(), 1u);
    EXPECT_EQ(e.book().best_level(Side::Buy)->total_qty, 10u);
}

TEST(Engine, HaltRejectsNewButAllowsCancel) {
    MatchingEngine e(small_book());
    run(e, limit(1, Side::Buy, 100, 10));
    e.set_halted(true);
    EXPECT_EQ(first(run(e, limit(2, Side::Buy, 101, 1)), EventType::Rejected)->reason,
              RejectReason::BookHalted);
    EXPECT_EQ(count(run(e, Command::make_cancel(1, 1)), EventType::Canceled), 1);
}

TEST(Engine, SelfTradePreventionCancelResting) {
    EngineConfig cfg = small_book();
    cfg.stp = StpMode::CancelResting;
    MatchingEngine e(cfg);
    run(e, Command::make_new(1, 7, Side::Buy, OrderType::Limit, 100, 5));
    auto evs = run(e, Command::make_new(2, 7, Side::Sell, OrderType::Limit, 100, 5));
    EXPECT_EQ(count(evs, EventType::Trade), 0);
    ASSERT_GE(count(evs, EventType::Canceled), 1);
    EXPECT_EQ(first(evs, EventType::Canceled)->reason, RejectReason::SelfTradePrevented);
}

TEST(Engine, BookChangedTracksTopOfBook) {
    MatchingEngine e(small_book());
    auto a = run(e, limit(1, Side::Buy, 100, 10));
    const Event* bc = first(a, EventType::BookChanged);
    ASSERT_NE(bc, nullptr);
    EXPECT_EQ(bc->bid_px, 100);
    // a deeper bid does not move the top -> no BookChanged
    auto b = run(e, limit(2, Side::Buy, 99, 10));
    EXPECT_EQ(count(b, EventType::BookChanged), 0);
}
