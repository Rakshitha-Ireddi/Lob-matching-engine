// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

// Commands entering the matching engine.
enum class CommandType : std::uint8_t { New = 0, Cancel = 1, Modify = 2 };

struct Command {
    CommandType type    = CommandType::New;
    OrderId     id      = 0;
    ClientId    client  = 0;
    Side        side    = Side::Buy;
    OrderType   ord_type = OrderType::Limit;
    Price       price   = kNoPrice;   // kNoPrice == market
    Quantity    quantity = 0;
    TsNanos     ts_recv = 0;          // set by the gateway on the wire

    static Command make_new(OrderId id, ClientId c, Side s, OrderType t, Price p,
                            Quantity q, TsNanos ts = 0) {
        return Command{CommandType::New, id, c, s, t, p, q, ts};
    }
    static Command make_cancel(OrderId id, ClientId c, TsNanos ts = 0) {
        Command cmd{};
        cmd.type = CommandType::Cancel;
        cmd.id = id;
        cmd.client = c;
        cmd.ts_recv = ts;
        return cmd;
    }
    static Command make_modify(OrderId id, ClientId c, Price new_price,
                               Quantity new_qty, TsNanos ts = 0) {
        Command cmd{};
        cmd.type = CommandType::Modify;
        cmd.id = id;
        cmd.client = c;
        cmd.price = new_price;
        cmd.quantity = new_qty;
        cmd.ts_recv = ts;
        return cmd;
    }
};

// Outbound events emitted while processing a command.
enum class EventType : std::uint8_t {
    Accepted    = 0,  // order acknowledged (resting or fully in-flight)
    Rejected    = 1,
    Fill        = 2,  // one execution leg (per order)
    Canceled    = 3,  // order removed (explicit cancel, IOC/FOK remainder, STP)
    Replaced    = 4,  // in-place modify kept queue priority
    Trade       = 5,  // public print: a match occurred (one per match)
    BookChanged = 6,  // top-of-book moved
};

struct Event {
    EventType    type      = EventType::Accepted;
    OrderId      order_id  = 0;
    OrderId      contra_id = 0;      // Fill: the resting counterparty
    ClientId     client    = 0;
    Side         side      = Side::Buy;
    Price        price     = kNoPrice;
    Quantity     quantity  = 0;      // Fill/Trade: matched size
    Quantity     leaves    = 0;      // open size after the event
    RejectReason reason    = RejectReason::None;
    bool         aggressor = false;  // Fill: this leg is the taker
    Sequence     seq       = 0;      // engine sequence number
    TsNanos      ts_in     = 0;      // command receive timestamp (carried through)
    TsNanos      ts_out    = 0;      // engine emit timestamp

    // BookChanged payload
    Price    bid_px  = kNoPrice;
    Quantity bid_qty = 0;
    Price    ask_px  = kNoPrice;
    Quantity ask_qty = 0;
};

}  // namespace lob
