// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <vector>

#include "lob/order.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {
Order mk(OrderId id, Side s, Price p, Quantity q) {
    Order o{};
    o.id = id;
    o.side = s;
    o.price = p;
    o.quantity = q;
    o.remaining = q;
    return o;
}
}  // namespace

TEST(OrderBook, EmptyBookHasNoBest) {
    OrderBook b(1, 1000);
    EXPECT_TRUE(b.empty(Side::Buy));
    EXPECT_TRUE(b.empty(Side::Sell));
    EXPECT_EQ(b.best_bid(), kNoPrice);
    EXPECT_EQ(b.best_ask(), kNoPrice);
    EXPECT_EQ(b.spread(), kNoPrice);
}

TEST(OrderBook, AddSetsBestByPricePriority) {
    OrderBook b(1, 1000);
    std::vector<Order> os;
    os.push_back(mk(1, Side::Buy, 100, 10));
    os.push_back(mk(2, Side::Buy, 105, 5));
    os.push_back(mk(3, Side::Buy, 102, 7));
    for (auto& o : os) b.add(&o);

    EXPECT_EQ(b.best_bid(), 105);
    EXPECT_EQ(b.order_count(Side::Buy), 3u);
    ASSERT_NE(b.best_level(Side::Buy), nullptr);
    EXPECT_EQ(b.best_level(Side::Buy)->total_qty, 5u);
}

TEST(OrderBook, AskBestIsLowest) {
    OrderBook b(1, 1000);
    Order a = mk(1, Side::Sell, 210, 3);
    Order c = mk(2, Side::Sell, 200, 4);
    Order d = mk(3, Side::Sell, 205, 1);
    b.add(&a);
    b.add(&c);
    b.add(&d);
    EXPECT_EQ(b.best_ask(), 200);
}

TEST(OrderBook, RemoveClearsLevelAndAdvancesBest) {
    OrderBook b(1, 1000);
    Order o1 = mk(1, Side::Buy, 100, 10);
    Order o2 = mk(2, Side::Buy, 99, 10);
    b.add(&o1);
    b.add(&o2);
    EXPECT_EQ(b.best_bid(), 100);
    b.remove(&o1);
    EXPECT_EQ(b.best_bid(), 99);
    b.remove(&o2);
    EXPECT_TRUE(b.empty(Side::Buy));
}

TEST(OrderBook, LevelAggregatesQuantityAndCount) {
    OrderBook b(1, 1000);
    Order o1 = mk(1, Side::Sell, 300, 4);
    Order o2 = mk(2, Side::Sell, 300, 6);
    b.add(&o1);
    b.add(&o2);
    const PriceLevel* l = b.level_at(Side::Sell, 300);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->count, 2u);
    EXPECT_EQ(l->total_qty, 10u);
    b.reduce_in_place(&o1, 3);
    EXPECT_EQ(l->total_qty, 7u);
    EXPECT_EQ(o1.remaining, 1u);
}

TEST(OrderBook, SpreadAndCrosses) {
    OrderBook b(1, 1000);
    Order bid = mk(1, Side::Buy, 100, 1);
    Order ask = mk(2, Side::Sell, 103, 1);
    b.add(&bid);
    b.add(&ask);
    EXPECT_EQ(b.spread(), 3);
    EXPECT_TRUE(OrderBook::crosses(Side::Buy, 103, 103));
    EXPECT_FALSE(OrderBook::crosses(Side::Buy, 102, 103));
    EXPECT_TRUE(OrderBook::crosses(Side::Sell, 100, 100));
    EXPECT_TRUE(OrderBook::crosses(Side::Buy, kNoPrice, 999));  // market
}

TEST(OrderBook, SnapshotWalksLevelsBestFirst) {
    OrderBook b(1, 1000);
    std::vector<Order> os;
    for (int i = 0; i < 5; ++i) os.push_back(mk(i + 1, Side::Buy, 100 - i, 10 + i));
    for (auto& o : os) b.add(&o);

    std::vector<OrderBook::DepthEntry> d;
    b.snapshot(Side::Buy, 3, d);
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0].price, 100);
    EXPECT_EQ(d[1].price, 99);
    EXPECT_EQ(d[2].price, 98);
}

TEST(OrderBook, MarketableQtyRespectsLimit) {
    OrderBook b(1, 1000);
    Order a1 = mk(1, Side::Sell, 100, 5);
    Order a2 = mk(2, Side::Sell, 101, 7);
    Order a3 = mk(3, Side::Sell, 103, 9);
    b.add(&a1);
    b.add(&a2);
    b.add(&a3);
    EXPECT_EQ(b.marketable_qty(Side::Buy, 101), 12u);   // 100 + 101
    EXPECT_EQ(b.marketable_qty(Side::Buy, 100), 5u);
    EXPECT_EQ(b.marketable_qty(Side::Buy, kNoPrice), 21u);
}

TEST(OrderBook, DeepBandBestPriceScan) {
    OrderBook b(1, 5'000'000);
    Order lo = mk(1, Side::Buy, 10, 1);
    Order hi = mk(2, Side::Buy, 4'999'000, 1);
    b.add(&lo);
    b.add(&hi);
    EXPECT_EQ(b.best_bid(), 4'999'000);
    b.remove(&hi);
    EXPECT_EQ(b.best_bid(), 10);
}
