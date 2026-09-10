// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "lob/clock.hpp"
#include "lob/events.hpp"
#include "lob/flat_hash_map.hpp"
#include "lob/object_pool.hpp"
#include "lob/order.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

struct EngineConfig {
    Price       min_price     = 1;
    Price       max_price     = 1'000'000;
    std::size_t max_orders    = 1'000'000;  // resting-order pool capacity
    StpMode     stp           = StpMode::None;
    bool        emit_book_changed = true;   // BookChanged events on every TOB move
};

struct EngineStats {
    std::uint64_t commands       = 0;
    std::uint64_t new_orders     = 0;
    std::uint64_t cancels        = 0;
    std::uint64_t modifies       = 0;
    std::uint64_t rejects        = 0;
    std::uint64_t trades         = 0;
    std::uint64_t shares_traded  = 0;
    std::uint64_t peak_resting   = 0;
};

// Single-threaded price-time-priority matching core.
//
// Not thread safe by design: exactly one thread ever calls process(). The
// gateway and market-data threads communicate with it through SPSC rings.
// See docs/adr/0004-single-writer-engine.md.
class MatchingEngine {
public:
    explicit MatchingEngine(const EngineConfig& cfg);

    // Process one command. Returns the events produced, valid until the next
    // call to process().
    std::span<const Event> process(const Command& cmd) noexcept;

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] Sequence sequence() const noexcept { return seq_; }
    [[nodiscard]] std::size_t resting_orders() const noexcept { return index_.size(); }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    void set_halted(bool h) noexcept { halted_ = h; }

private:
    void handle_new(const Command& cmd) noexcept;
    void handle_cancel(const Command& cmd) noexcept;
    void handle_modify(const Command& cmd) noexcept;

    // Match `incoming` against the resting book; returns filled quantity.
    Quantity match(Order& incoming, ClientId client) noexcept;

    Event& new_event(EventType t) noexcept;
    void emit_reject(const Command& cmd, RejectReason r) noexcept;
    void emit_book_changed(TsNanos ts_in) noexcept;

    EngineConfig cfg_;
    OrderBook book_;
    ObjectPool<Order> pool_;
    FlatPtrMap<Order*> index_;

    std::vector<Event> out_;
    Sequence seq_ = 0;         // outbound event sequence
    Sequence arrival_ = 0;     // accepted-order arrival counter (time priority)
    TsNanos cur_ts_out_ = 0;   // one clock read per process() call
    EngineStats stats_;
    bool halted_ = false;

    Price last_bid_ = kNoPrice;
    Price last_ask_ = kNoPrice;
};

}  // namespace lob
