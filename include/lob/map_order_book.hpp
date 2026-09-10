// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <vector>

#include "lob/order.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Baseline order book: one ordered `std::map<Price, PriceLevel>` per side.
//
// The textbook implementation. Level lookup, insert and erase are O(log n);
// the best price is `begin()`, O(1). It exists as the comparison baseline for
// the bitset book -- identical public surface and the same PriceLevel FIFO, so
// `BasicMatchingEngine` runs against either unchanged. See docs/benchmarks.md.
class MapOrderBook {
public:
    MapOrderBook(Price min_price, Price max_price)
        : min_price_(min_price), max_price_(max_price) {}

    [[nodiscard]] bool in_band(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    void add(Order* o) {
        PriceLevel& lvl =
            (o->side == Side::Buy) ? bid_[o->price] : ask_[o->price];
        if (lvl.head == nullptr) lvl.price = o->price;
        lvl.push_back(o);
        ++count_[side_idx(o->side)];
    }

    void remove(Order* o) {
        if (o->side == Side::Buy) {
            auto it = bid_.find(o->price);
            it->second.unlink(o);
            if (it->second.empty()) bid_.erase(it);
        } else {
            auto it = ask_.find(o->price);
            it->second.unlink(o);
            if (it->second.empty()) ask_.erase(it);
        }
        --count_[side_idx(o->side)];
    }

    void reduce_in_place(Order* o, Quantity traded) noexcept {
        PriceLevel& lvl = (o->side == Side::Buy) ? bid_.find(o->price)->second
                                                 : ask_.find(o->price)->second;
        lvl.total_qty -= traded;
        o->remaining -= traded;
    }

    [[nodiscard]] PriceLevel* best_level(Side s) noexcept {
        if (s == Side::Buy) return bid_.empty() ? nullptr : &bid_.begin()->second;
        return ask_.empty() ? nullptr : &ask_.begin()->second;
    }
    [[nodiscard]] const PriceLevel* best_level(Side s) const noexcept {
        if (s == Side::Buy) return bid_.empty() ? nullptr : &bid_.begin()->second;
        return ask_.empty() ? nullptr : &ask_.begin()->second;
    }

    [[nodiscard]] Price best_price(Side s) const noexcept {
        const PriceLevel* l = best_level(s);
        return l ? l->price : kNoPrice;
    }
    [[nodiscard]] Price best_bid() const noexcept { return best_price(Side::Buy); }
    [[nodiscard]] Price best_ask() const noexcept { return best_price(Side::Sell); }
    [[nodiscard]] Price spread() const noexcept {
        const Price b = best_bid();
        const Price a = best_ask();
        return (b == kNoPrice || a == kNoPrice) ? kNoPrice : a - b;
    }

    [[nodiscard]] const PriceLevel* level_at(Side s, Price p) const noexcept {
        if (s == Side::Buy) {
            auto it = bid_.find(p);
            return it == bid_.end() ? nullptr : &it->second;
        }
        auto it = ask_.find(p);
        return it == ask_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool empty(Side s) const noexcept { return count_[side_idx(s)] == 0; }
    [[nodiscard]] std::size_t order_count(Side s) const noexcept {
        return count_[side_idx(s)];
    }
    [[nodiscard]] std::size_t order_count() const noexcept {
        return count_[0] + count_[1];
    }

    [[nodiscard]] Quantity marketable_qty(Side side, Price limit) const noexcept {
        Quantity total = 0;
        if (opposite(side) == Side::Buy) {
            for (const auto& [px, lvl] : bid_) {
                if (!crosses(side, limit, px)) break;
                total += lvl.total_qty;
            }
        } else {
            for (const auto& [px, lvl] : ask_) {
                if (!crosses(side, limit, px)) break;
                total += lvl.total_qty;
            }
        }
        return total;
    }

    void snapshot(Side s, std::size_t max_levels, std::vector<DepthEntry>& out) const {
        out.clear();
        if (s == Side::Buy) {
            for (const auto& [px, lvl] : bid_) {
                if (out.size() >= max_levels) break;
                out.push_back({px, lvl.total_qty, lvl.count});
            }
        } else {
            for (const auto& [px, lvl] : ask_) {
                if (out.size() >= max_levels) break;
                out.push_back({px, lvl.total_qty, lvl.count});
            }
        }
    }

private:
    static constexpr int side_idx(Side s) noexcept { return static_cast<int>(s); }

    Price min_price_;
    Price max_price_;
    std::map<Price, PriceLevel, std::greater<Price>> bid_;  // begin() == best bid
    std::map<Price, PriceLevel, std::less<Price>> ask_;     // begin() == best ask
    std::size_t count_[2] = {0, 0};
};

}  // namespace lob
