// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace lob {

// ---------------------------------------------------------------------------
// Primitive domain types.
//
// Prices are integer tick counts (fixed point). Never floating point in the
// hot path -- see docs/adr/0003-fixed-point-prices.md.
// ---------------------------------------------------------------------------
using OrderId  = std::uint64_t;
using ClientId = std::uint32_t;
using Price    = std::int64_t;   // number of ticks from zero
using Quantity = std::uint64_t;  // lots / shares
using Sequence = std::uint64_t;  // global arrival sequence -> time priority
using TsNanos  = std::uint64_t;  // nanoseconds in the monotonic clock domain

inline constexpr Price kNoPrice = INT64_MIN;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Does an aggressor on `side` at `limit` cross a resting level priced at
// `level_price`? A market order (limit == kNoPrice) always crosses. Shared by
// every order-book implementation.
[[nodiscard]] inline constexpr bool crosses(Side side, Price limit,
                                            Price level_price) noexcept {
    if (limit == kNoPrice) return true;
    return side == Side::Buy ? limit >= level_price : limit <= level_price;
}

// Order handling instructions.
enum class OrderType : std::uint8_t {
    Limit    = 0,  // rest the unfilled remainder
    Market   = 1,  // sweep the book, cancel any remainder
    Ioc      = 2,  // immediate-or-cancel: match now, cancel remainder (limit price)
    PostOnly = 3,  // must rest; reject if marketable
    Fok      = 4,  // fill-or-kill: fill in full immediately or reject
};

// Self-trade prevention policy applied when an aggressor meets its own resting
// order (matched on ClientId).
enum class StpMode : std::uint8_t {
    None            = 0,
    CancelResting   = 1,  // pull the resting order, keep matching
    CancelAggressor = 2,  // stop the incoming order, leave the book untouched
};

enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownOrder,
    DuplicateOrderId,
    PriceOutOfBand,
    ZeroQuantity,
    PostOnlyWouldCross,
    FokUnfillable,
    BookHalted,
    PoolExhausted,
    SelfTradePrevented,
};

[[nodiscard]] const char* to_string(Side) noexcept;
[[nodiscard]] const char* to_string(OrderType) noexcept;
[[nodiscard]] const char* to_string(RejectReason) noexcept;

}  // namespace lob
