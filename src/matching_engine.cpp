// SPDX-License-Identifier: MIT
#include "lob/matching_engine.hpp"

#include <algorithm>

namespace lob {

MatchingEngine::MatchingEngine(const EngineConfig& cfg)
    : cfg_(cfg),
      book_(cfg.min_price, cfg.max_price),
      pool_(cfg.max_orders),
      index_(cfg.max_orders) {
    out_.reserve(64);
}

Event& MatchingEngine::new_event(EventType t) noexcept {
    out_.emplace_back();
    Event& e = out_.back();
    e.type = t;
    e.seq = ++seq_;
    e.ts_out = cur_ts_out_;
    return e;
}

void MatchingEngine::emit_reject(const Command& cmd, RejectReason r) noexcept {
    ++stats_.rejects;
    Event& e = new_event(EventType::Rejected);
    e.order_id = cmd.id;
    e.client = cmd.client;
    e.side = cmd.side;
    e.price = cmd.price;
    e.quantity = cmd.quantity;
    e.leaves = 0;
    e.reason = r;
    e.ts_in = cmd.ts_recv;
}

void MatchingEngine::emit_book_changed(TsNanos ts_in) noexcept {
    const Price bid = book_.best_bid();
    const Price ask = book_.best_ask();
    if (bid == last_bid_ && ask == last_ask_) return;
    last_bid_ = bid;
    last_ask_ = ask;

    Event& e = new_event(EventType::BookChanged);
    e.ts_in = ts_in;
    e.bid_px = bid;
    e.ask_px = ask;
    const PriceLevel* bl = book_.best_level(Side::Buy);
    const PriceLevel* al = book_.best_level(Side::Sell);
    e.bid_qty = bl ? bl->total_qty : 0;
    e.ask_qty = al ? al->total_qty : 0;
}

std::span<const Event> MatchingEngine::process(const Command& cmd) noexcept {
    out_.clear();
    cur_ts_out_ = now_ns();
    ++stats_.commands;

    switch (cmd.type) {
        case CommandType::New:
            ++stats_.new_orders;
            if (halted_) {
                emit_reject(cmd, RejectReason::BookHalted);
            } else {
                handle_new(cmd);
            }
            break;
        case CommandType::Cancel:
            ++stats_.cancels;
            handle_cancel(cmd);
            break;
        case CommandType::Modify:
            ++stats_.modifies;
            handle_modify(cmd);
            break;
    }

    if (cfg_.emit_book_changed) emit_book_changed(cmd.ts_recv);

    pool_.tick_high_water();
    stats_.peak_resting = std::max<std::uint64_t>(stats_.peak_resting, index_.size());
    return {out_.data(), out_.size()};
}

Quantity MatchingEngine::match(Order& in, ClientId client) noexcept {
    const Side book_side = opposite(in.side);
    Quantity total_filled = 0;

    while (in.remaining > 0) {
        PriceLevel* lvl = book_.best_level(book_side);
        if (lvl == nullptr) break;
        if (in.type != OrderType::Market &&
            !OrderBook::crosses(in.side, in.price, lvl->price)) {
            break;
        }

        Order* resting = lvl->head;
        while (resting != nullptr && in.remaining > 0) {
            Order* next = resting->next;

            if (cfg_.stp != StpMode::None && resting->client == client) {
                if (cfg_.stp == StpMode::CancelAggressor) {
                    Event& e = new_event(EventType::Canceled);
                    e.order_id = in.id;
                    e.client = client;
                    e.side = in.side;
                    e.price = in.price;
                    e.leaves = 0;
                    e.reason = RejectReason::SelfTradePrevented;
                    e.ts_in = in.ts_recv;
                    in.remaining = 0;
                    return total_filled;
                }
                Event& e = new_event(EventType::Canceled);
                e.order_id = resting->id;
                e.client = resting->client;
                e.side = resting->side;
                e.price = resting->price;
                e.leaves = 0;
                e.reason = RejectReason::SelfTradePrevented;
                e.ts_in = in.ts_recv;
                book_.remove(resting);
                index_.erase(resting->id);
                pool_.release(resting);
                resting = next;
                continue;
            }

            const Quantity traded = std::min(in.remaining, resting->remaining);
            const Price px = resting->price;

            {
                Event& e = new_event(EventType::Fill);
                e.order_id = in.id;
                e.contra_id = resting->id;
                e.client = in.client;
                e.side = in.side;
                e.price = px;
                e.quantity = traded;
                e.leaves = in.remaining - traded;
                e.aggressor = true;
                e.ts_in = in.ts_recv;
            }
            {
                Event& e = new_event(EventType::Fill);
                e.order_id = resting->id;
                e.contra_id = in.id;
                e.client = resting->client;
                e.side = resting->side;
                e.price = px;
                e.quantity = traded;
                e.leaves = resting->remaining - traded;
                e.aggressor = false;
                e.ts_in = in.ts_recv;
            }
            {
                Event& e = new_event(EventType::Trade);
                e.order_id = in.id;
                e.contra_id = resting->id;
                e.side = in.side;  // aggressor side
                e.price = px;
                e.quantity = traded;
                e.ts_in = in.ts_recv;
            }

            in.remaining -= traded;
            total_filled += traded;
            ++stats_.trades;
            stats_.shares_traded += traded;

            if (resting->remaining == traded) {
                book_.remove(resting);
                index_.erase(resting->id);
                pool_.release(resting);
            } else {
                book_.reduce_in_place(resting, traded);
            }
            resting = next;
        }
    }
    return total_filled;
}

void MatchingEngine::handle_new(const Command& cmd) noexcept {
    const bool is_market =
        cmd.ord_type == OrderType::Market || cmd.price == kNoPrice;
    const Price limit = is_market ? kNoPrice : cmd.price;

    if (cmd.quantity == 0) {
        emit_reject(cmd, RejectReason::ZeroQuantity);
        return;
    }
    if (!is_market && !book_.in_band(cmd.price)) {
        emit_reject(cmd, RejectReason::PriceOutOfBand);
        return;
    }
    if (index_.find(cmd.id) != nullptr) {
        emit_reject(cmd, RejectReason::DuplicateOrderId);
        return;
    }
    if (cmd.ord_type == OrderType::PostOnly) {
        const PriceLevel* opp = book_.best_level(opposite(cmd.side));
        if (opp != nullptr && OrderBook::crosses(cmd.side, limit, opp->price)) {
            emit_reject(cmd, RejectReason::PostOnlyWouldCross);
            return;
        }
    }
    if (cmd.ord_type == OrderType::Fok) {
        if (book_.marketable_qty(cmd.side, limit) < cmd.quantity) {
            emit_reject(cmd, RejectReason::FokUnfillable);
            return;
        }
    }

    Order incoming{};
    incoming.id = cmd.id;
    incoming.client = cmd.client;
    incoming.price = limit;
    incoming.quantity = cmd.quantity;
    incoming.remaining = cmd.quantity;
    incoming.side = cmd.side;
    incoming.type = cmd.ord_type;
    incoming.ts_recv = cmd.ts_recv;
    incoming.seq = ++arrival_;

    {
        Event& e = new_event(EventType::Accepted);
        e.order_id = cmd.id;
        e.client = cmd.client;
        e.side = cmd.side;
        e.price = limit;
        e.quantity = cmd.quantity;
        e.leaves = cmd.quantity;
        e.aggressor = true;
        e.ts_in = cmd.ts_recv;
    }

    match(incoming, cmd.client);

    if (incoming.remaining == 0) {
        return;  // fully filled
    }

    const bool rests =
        incoming.type == OrderType::Limit || incoming.type == OrderType::PostOnly;
    if (!rests) {
        Event& e = new_event(EventType::Canceled);
        e.order_id = cmd.id;
        e.client = cmd.client;
        e.side = cmd.side;
        e.price = limit;
        e.quantity = incoming.remaining;
        e.leaves = 0;
        e.ts_in = cmd.ts_recv;
        return;
    }

    Order* slot = pool_.acquire();
    if (slot == nullptr) {
        Event& e = new_event(EventType::Canceled);
        e.order_id = cmd.id;
        e.client = cmd.client;
        e.side = cmd.side;
        e.price = limit;
        e.quantity = incoming.remaining;
        e.leaves = 0;
        e.reason = RejectReason::PoolExhausted;
        e.ts_in = cmd.ts_recv;
        return;
    }
    *slot = incoming;
    book_.add(slot);
    index_.insert(slot->id, slot);
}

void MatchingEngine::handle_cancel(const Command& cmd) noexcept {
    Order* o = index_.find(cmd.id);
    if (o == nullptr) {
        emit_reject(cmd, RejectReason::UnknownOrder);
        return;
    }
    Event& e = new_event(EventType::Canceled);
    e.order_id = o->id;
    e.client = o->client;
    e.side = o->side;
    e.price = o->price;
    e.quantity = o->remaining;
    e.leaves = 0;
    e.ts_in = cmd.ts_recv;

    book_.remove(o);
    index_.erase(cmd.id);
    pool_.release(o);
}

void MatchingEngine::handle_modify(const Command& cmd) noexcept {
    Order* o = index_.find(cmd.id);
    if (o == nullptr) {
        emit_reject(cmd, RejectReason::UnknownOrder);
        return;
    }

    if (cmd.quantity == 0) {
        Command c = Command::make_cancel(cmd.id, cmd.client, cmd.ts_recv);
        handle_cancel(c);
        return;
    }

    const Price old_price = o->price;
    const Price new_price = (cmd.price == kNoPrice) ? old_price : cmd.price;
    const Quantity new_qty = cmd.quantity;

    // Size-down at the same price is the only change that keeps queue
    // position -- see docs/adr/0005-modify-semantics.md.
    if (new_price == old_price && new_qty < o->remaining) {
        const Quantity delta = o->remaining - new_qty;
        book_.reduce_in_place(o, delta);
        o->quantity = new_qty + o->filled();

        Event& e = new_event(EventType::Replaced);
        e.order_id = o->id;
        e.client = o->client;
        e.side = o->side;
        e.price = o->price;
        e.quantity = new_qty;
        e.leaves = o->remaining;
        e.ts_in = cmd.ts_recv;
        return;
    }

    const ClientId client = o->client;
    const Side side = o->side;
    const OrderType type = o->type;

    book_.remove(o);
    index_.erase(cmd.id);
    pool_.release(o);

    Command repl =
        Command::make_new(cmd.id, client, side, type, new_price, new_qty, cmd.ts_recv);
    handle_new(repl);
}

}  // namespace lob
