// SPDX-License-Identifier: MIT
//
// The three order-book implementations must be behaviourally identical: fed the
// same command stream, BasicMatchingEngine<Book> produces the same trades and
// the same resulting book for every Book. This is what lets docs/benchmarks.md
// compare them as pure data-structure swaps.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"
#include "lob/order_flow.hpp"
#include "lob/sorted_vector_order_book.hpp"

using namespace lob;

namespace {

struct Trace {
    std::uint64_t events = 0;
    std::uint64_t trades = 0;
    std::uint64_t shares = 0;
    std::uint64_t rejects = 0;
    std::uint64_t fnv = 1469598103934665603ULL;
    Price final_bid = 0;
    Price final_ask = 0;
    Price final_spread = 0;
    std::size_t resting = 0;

    void mix(std::uint64_t v) {
        fnv ^= v;
        fnv *= 1099511628211ULL;
    }
    bool operator==(const Trace&) const = default;
};

template <class Book>
Trace replay(std::uint64_t seed, std::uint64_t n) {
    EngineConfig ec;
    ec.min_price = 1;
    ec.max_price = 60'000;
    ec.max_orders = 400'000;

    FlowConfig fc;
    fc.seed = seed;
    fc.ref_price = 30'000;
    fc.min_price = 1;
    fc.max_price = 60'000;
    fc.max_live = 20'000;

    BasicMatchingEngine<Book> eng(ec);
    OrderFlowGenerator gen(fc);

    Trace t;
    for (std::uint64_t i = 0; i < n; ++i) {
        const Command c = gen.next();
        for (const Event& e : eng.process(c)) {
            ++t.events;
            t.mix(static_cast<std::uint64_t>(e.type));
            t.mix(e.order_id);
            t.mix(e.contra_id);
            t.mix(static_cast<std::uint64_t>(e.price + 1));
            t.mix(e.quantity);
            t.mix(e.leaves);
        }
    }
    const auto& s = eng.stats();
    t.trades = s.trades;
    t.shares = s.shares_traded;
    t.rejects = s.rejects;
    t.final_bid = eng.book().best_bid();
    t.final_ask = eng.book().best_ask();
    t.final_spread = eng.book().spread();
    t.resting = eng.resting_orders();
    return t;
}

}  // namespace

TEST(BookEquivalence, MapMatchesBitset) {
    for (std::uint64_t seed : {1u, 7u, 42u, 999u}) {
        const Trace a = replay<OrderBook>(seed, 120'000);
        const Trace b = replay<MapOrderBook>(seed, 120'000);
        EXPECT_EQ(a, b) << "seed " << seed;
        EXPECT_GT(a.trades, 0u);
    }
}

TEST(BookEquivalence, SortedVectorMatchesBitset) {
    for (std::uint64_t seed : {1u, 7u, 42u, 999u}) {
        const Trace a = replay<OrderBook>(seed, 120'000);
        const Trace c = replay<SortedVectorOrderBook>(seed, 120'000);
        EXPECT_EQ(a, c) << "seed " << seed;
    }
}

TEST(BookEquivalence, AllFourAgreeOnALongRun) {
    const Trace a = replay<OrderBook>(2024, 400'000);
    EXPECT_EQ(a, replay<MapOrderBook>(2024, 400'000));
    EXPECT_EQ(a, replay<PooledMapOrderBook>(2024, 400'000));
    EXPECT_EQ(a, replay<SortedVectorOrderBook>(2024, 400'000));
    EXPECT_GT(a.events, a.trades);
}

TEST(BookEquivalence, PooledMapMatchesMap) {
    for (std::uint64_t seed : {1u, 42u, 777u}) {
        EXPECT_EQ(replay<MapOrderBook>(seed, 150'000),
                  replay<PooledMapOrderBook>(seed, 150'000))
            << "seed " << seed;
    }
}
