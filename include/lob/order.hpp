// SPDX-License-Identifier: MIT
#pragma once

#include "lob/types.hpp"

namespace lob {

// A single resting or in-flight order.
//
// `prev`/`next` make the order an intrusive node in its price level's FIFO
// queue, so resting and cancelling touch no allocator and no separate list
// node -- see docs/adr/0002-book-data-structure.md.
struct Order {
    OrderId  id        = 0;
    ClientId client    = 0;
    Price    price     = 0;
    Quantity quantity  = 0;  // original size
    Quantity remaining = 0;  // open size
    Sequence seq       = 0;  // arrival order within the engine
    TsNanos  ts_recv   = 0;  // gateway receive timestamp
    Side     side      = Side::Buy;
    OrderType type     = OrderType::Limit;

    Order* prev = nullptr;
    Order* next = nullptr;

    [[nodiscard]] Quantity filled() const noexcept { return quantity - remaining; }
    [[nodiscard]] bool resting() const noexcept { return remaining > 0; }

    void reset() noexcept { *this = Order{}; }
};

}  // namespace lob
