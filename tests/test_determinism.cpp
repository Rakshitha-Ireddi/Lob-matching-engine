// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/matching_engine.hpp"
#include "lob/order_flow.hpp"

using namespace lob;

namespace {

struct Digest {
    std::uint64_t commands = 0;
    std::uint64_t trades = 0;
    std::uint64_t shares = 0;
    std::uint64_t rejects = 0;
    std::uint64_t trade_hash = 1469598103934665603ULL;  // FNV-ish
    Price final_bid = 0;
    Price final_ask = 0;
    std::size_t resting = 0;

    void mix(std::uint64_t v) {
        trade_hash ^= v;
        trade_hash *= 1099511628211ULL;
    }
    bool operator==(const Digest&) const = default;
};

Digest replay(std::uint64_t seed, std::uint64_t n) {
    EngineConfig ecfg;
    ecfg.min_price = 1;
    ecfg.max_price = 40'000;
    ecfg.max_orders = 500'000;

    FlowConfig fcfg;
    fcfg.seed = seed;
    fcfg.ref_price = 20'000;
    fcfg.min_price = 1;
    fcfg.max_price = 40'000;

    MatchingEngine engine(ecfg);
    OrderFlowGenerator gen(fcfg);

    Digest d;
    for (std::uint64_t i = 0; i < n; ++i) {
        Command c = gen.next();
        auto evs = engine.process(c);
        for (const Event& e : evs) {
            if (e.type == EventType::Trade) {
                d.mix(static_cast<std::uint64_t>(e.price));
                d.mix(e.quantity);
                d.mix(e.order_id);
                d.mix(e.contra_id);
            }
        }
    }
    const auto& s = engine.stats();
    d.commands = s.commands;
    d.trades = s.trades;
    d.shares = s.shares_traded;
    d.rejects = s.rejects;
    d.final_bid = engine.book().best_bid();
    d.final_ask = engine.book().best_ask();
    d.resting = engine.resting_orders();
    return d;
}

}  // namespace

TEST(Determinism, SameSeedSameHistory) {
    EXPECT_EQ(replay(42, 200'000), replay(42, 200'000));
}

TEST(Determinism, DifferentSeedDifferentHistory) {
    EXPECT_NE(replay(1, 100'000), replay(2, 100'000));
}

TEST(Determinism, GeneratorAloneIsReproducible) {
    FlowConfig f;
    f.seed = 99;
    OrderFlowGenerator a(f), b(f);
    for (int i = 0; i < 50'000; ++i) {
        Command x = a.next();
        Command y = b.next();
        EXPECT_EQ(x.type, y.type);
        EXPECT_EQ(x.id, y.id);
        EXPECT_EQ(x.side, y.side);
        EXPECT_EQ(x.price, y.price);
        EXPECT_EQ(x.quantity, y.quantity);
    }
}

TEST(Determinism, ResetRewindsGenerator) {
    FlowConfig f;
    f.seed = 7;
    OrderFlowGenerator g(f);
    std::vector<OrderId> first;
    for (int i = 0; i < 1000; ++i) first.push_back(g.next().id);
    g.reset();
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(g.next().id, first[i]);
}
