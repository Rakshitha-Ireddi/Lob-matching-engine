// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "lob/order.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Baseline order book: one price-sorted `std::vector` per side (the layout a
// `boost::flat_map` would give -- contiguous, cache-friendly to scan, O(log n)
// to search, but O(n) to insert or erase a price level).
//
// Bids are kept descending and asks ascending, so `front()` is always the best
// price (O(1)). Same public surface and PriceLevel FIFO as the other books.
// See docs/benchmarks.md.
class SortedVectorOrderBook {
    struct Node {
        Price price;
        PriceLevel level;
    };

    static constexpr int side_idx(Side s) noexcept { return static_cast<int>(s); }

    // First node whose price is not "better than" `price` for that side:
    //   bids  (descending): first with price <= target
    //   asks  (ascending):  first with price >= target
    template <class Vec>
    static auto seek(Vec& v, Side s, Price price) {
        if (s == Side::Buy) {
            return std::lower_bound(v.begin(), v.end(), price,
                                    [](const Node& n, Price p) { return n.price > p; });
        }
        return std::lower_bound(v.begin(), v.end(), price,
                                [](const Node& n, Price p) { return n.price < p; });
    }

public:
    SortedVectorOrderBook(Price min_price, Price max_price)
        : min_price_(min_price), max_price_(max_price) {
        bid_.reserve(1024);
        ask_.reserve(1024);
    }

    [[nodiscard]] bool in_band(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    void add(Order* o) {
        std::vector<Node>& v = (o->side == Side::Buy) ? bid_ : ask_;
        const auto it = seek(v, o->side, o->price);
        if (it != v.end() && it->price == o->price) {
            it->level.push_back(o);
        } else {
            const auto pos = v.insert(it, Node{o->price, PriceLevel{}});
            pos->level.price = o->price;
            pos->level.push_back(o);
        }
        ++count_[side_idx(o->side)];
    }

    void remove(Order* o) {
        std::vector<Node>& v = (o->side == Side::Buy) ? bid_ : ask_;
        const auto it = seek(v, o->side, o->price);
        it->level.unlink(o);
        --count_[side_idx(o->side)];
        if (it->level.empty()) v.erase(it);
    }

    void reduce_in_place(Order* o, Quantity traded) noexcept {
        std::vector<Node>& v = (o->side == Side::Buy) ? bid_ : ask_;
        seek(v, o->side, o->price)->level.total_qty -= traded;
        o->remaining -= traded;
    }

    [[nodiscard]] PriceLevel* best_level(Side s) noexcept {
        std::vector<Node>& v = (s == Side::Buy) ? bid_ : ask_;
        return v.empty() ? nullptr : &v.front().level;
    }
    [[nodiscard]] const PriceLevel* best_level(Side s) const noexcept {
        const std::vector<Node>& v = (s == Side::Buy) ? bid_ : ask_;
        return v.empty() ? nullptr : &v.front().level;
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
        const std::vector<Node>& v = (s == Side::Buy) ? bid_ : ask_;
        const auto it = seek(v, s, p);
        return (it != v.end() && it->price == p) ? &it->level : nullptr;
    }

    [[nodiscard]] bool empty(Side s) const noexcept { return count_[side_idx(s)] == 0; }
    [[nodiscard]] std::size_t order_count(Side s) const noexcept {
        return count_[side_idx(s)];
    }
    [[nodiscard]] std::size_t order_count() const noexcept {
        return count_[0] + count_[1];
    }

    [[nodiscard]] Quantity marketable_qty(Side side, Price limit) const noexcept {
        const std::vector<Node>& v = (opposite(side) == Side::Buy) ? bid_ : ask_;
        Quantity total = 0;
        for (const Node& n : v) {
            if (!crosses(side, limit, n.price)) break;
            total += n.level.total_qty;
        }
        return total;
    }

    void snapshot(Side s, std::size_t max_levels, std::vector<DepthEntry>& out) const {
        out.clear();
        const std::vector<Node>& v = (s == Side::Buy) ? bid_ : ask_;
        for (const Node& n : v) {
            if (out.size() >= max_levels) break;
            out.push_back({n.price, n.level.total_qty, n.level.count});
        }
    }

private:
    Price min_price_;
    Price max_price_;
    std::vector<Node> bid_;  // price descending, front() == best bid
    std::vector<Node> ask_;  // price ascending,  front() == best ask
    std::size_t count_[2] = {0, 0};
};

}  // namespace lob
