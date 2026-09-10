// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// Deterministic synthetic order-flow generator.
//
// Produces a stream of NEW / CANCEL / MODIFY commands around a slowly drifting
// mid price with a configurable action mix and aggression. Given the same
// seed and config it yields the exact same command sequence, which is what
// the determinism test and reproducible benchmarks rely on.
struct FlowConfig {
    std::uint64_t seed        = 42;
    Price    ref_price        = 10'000;   // starting mid, in ticks
    Price    min_price        = 1;
    Price    max_price        = 20'000;
    Quantity min_qty          = 1;
    Quantity max_qty          = 100;
    double   p_new            = 0.60;     // action mix (rest is cancel/modify)
    double   p_cancel         = 0.32;
    double   p_market         = 0.02;     // of NEWs: marketable market orders
    double   p_aggressive     = 0.14;     // of NEW limits: cross the spread
    int      depth_ticks      = 16;       // limit price scatter around mid
    double   drift_prob       = 0.04;     // chance mid steps each command
    ClientId clients          = 8;
    std::size_t max_live      = 60'000;   // cap on believed-resting orders (book depth)
};

class OrderFlowGenerator {
public:
    explicit OrderFlowGenerator(const FlowConfig& cfg);

    [[nodiscard]] Command next() noexcept;

    [[nodiscard]] Price mid() const noexcept { return mid_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return live_.size(); }
    [[nodiscard]] OrderId last_id() const noexcept { return next_id_ - 1; }

    void reset();

private:
    struct Live {
        OrderId  id;
        ClientId client;
    };

    [[nodiscard]] std::uint64_t next_u64() noexcept;
    [[nodiscard]] double unit() noexcept;        // [0, 1)
    [[nodiscard]] std::uint64_t range(std::uint64_t n) noexcept;

    Command make_new() noexcept;
    void track(OrderId id, ClientId c);
    bool untrack_random(Live& out) noexcept;
    bool peek_random(Live& out) noexcept;

    FlowConfig cfg_;
    std::uint64_t s_[4];
    OrderId next_id_ = 1;
    Price mid_;

    std::vector<Live> live_;  // orders we believe are resting; swap-removed at random
};

}  // namespace lob
