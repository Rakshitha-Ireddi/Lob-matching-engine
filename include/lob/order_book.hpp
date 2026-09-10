// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "lob/order.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time-priority book for a single instrument.
//
// Each side is a dense array of PriceLevel indexed by (price - min_price), so
// looking up the level for a known price is a single load. A two-level
// occupancy bitset over that array turns "find the best price" into a couple
// of hardware bit-scans instead of a tree walk.
//
//   words_[side]  : one bit per price level, set when the level is non-empty
//   summary_[side]: one bit per 64-level word, set when that word is non-zero
//
// See docs/adr/0002-book-data-structure.md.
class OrderBook {
public:
    OrderBook(Price min_price, Price max_price)
        : min_price_(min_price),
          max_price_(max_price),
          span_(static_cast<std::size_t>(max_price - min_price) + 1) {
        assert(max_price >= min_price);
        for (int s = 0; s < 2; ++s) {
            levels_[s].assign(span_, PriceLevel{});
            words_[s].assign((span_ + 63) / 64, 0ULL);
            summary_[s].assign((words_[s].size() + 63) / 64, 0ULL);
        }
    }

    [[nodiscard]] Price min_price() const noexcept { return min_price_; }
    [[nodiscard]] Price max_price() const noexcept { return max_price_; }

    [[nodiscard]] bool in_band(Price p) const noexcept {
        return p >= min_price_ && p <= max_price_;
    }

    // --- mutation ---------------------------------------------------------

    void add(Order* o) noexcept {
        const std::size_t i = index(o->price);
        PriceLevel& lvl = levels_[side_idx(o->side)][i];
        const bool was_empty = lvl.empty();
        if (was_empty) lvl.price = o->price;
        lvl.push_back(o);
        if (was_empty) set_bit(side_idx(o->side), i);
        order_count_[side_idx(o->side)] += 1;
    }

    void remove(Order* o) noexcept {
        const std::size_t i = index(o->price);
        const int s = side_idx(o->side);
        PriceLevel& lvl = levels_[s][i];
        lvl.unlink(o);
        order_count_[s] -= 1;
        if (lvl.empty()) {
            lvl.price = kNoPrice;
            clear_bit(s, i);
        }
    }

    // Resting order was partially hit by an aggressor.
    void reduce_in_place(Order* o, Quantity traded) noexcept {
        levels_[side_idx(o->side)][index(o->price)].total_qty -= traded;
        o->remaining -= traded;
    }

    // --- queries ---------------------------------------------------------

    [[nodiscard]] bool empty(Side s) const noexcept {
        return order_count_[side_idx(s)] == 0;
    }
    [[nodiscard]] std::size_t order_count(Side s) const noexcept {
        return order_count_[side_idx(s)];
    }
    [[nodiscard]] std::size_t order_count() const noexcept {
        return order_count_[0] + order_count_[1];
    }

    [[nodiscard]] Price best_price(Side s) const noexcept {
        const std::int64_t bi = (s == Side::Buy) ? highest_set(side_idx(s))
                                                 : lowest_set(side_idx(s));
        return bi < 0 ? kNoPrice : min_price_ + bi;
    }

    [[nodiscard]] const PriceLevel* best_level(Side s) const noexcept {
        const std::int64_t bi = (s == Side::Buy) ? highest_set(side_idx(s))
                                                 : lowest_set(side_idx(s));
        return bi < 0 ? nullptr : &levels_[side_idx(s)][static_cast<std::size_t>(bi)];
    }
    [[nodiscard]] PriceLevel* best_level(Side s) noexcept {
        return const_cast<PriceLevel*>(std::as_const(*this).best_level(s));
    }

    [[nodiscard]] const PriceLevel* level_at(Side s, Price p) const noexcept {
        if (!in_band(p)) return nullptr;
        const PriceLevel& lvl = levels_[side_idx(s)][index(p)];
        return lvl.empty() ? nullptr : &lvl;
    }

    [[nodiscard]] Price best_bid() const noexcept { return best_price(Side::Buy); }
    [[nodiscard]] Price best_ask() const noexcept { return best_price(Side::Sell); }

    [[nodiscard]] Price spread() const noexcept {
        const Price b = best_bid();
        const Price a = best_ask();
        return (b == kNoPrice || a == kNoPrice) ? kNoPrice : a - b;
    }

    // Convenience forwarder to lob::crosses (kept for call sites that spell it
    // OrderBook::crosses).
    [[nodiscard]] static bool crosses(Side side, Price limit, Price level_price) noexcept {
        return lob::crosses(side, limit, level_price);
    }

    // Total resting quantity an aggressor on `side` could take at `limit`,
    // walking the opposite side from the top. Used for fill-or-kill sizing.
    [[nodiscard]] Quantity marketable_qty(Side side, Price limit) const noexcept {
        const Side book_side = opposite(side);
        Quantity total = 0;
        for (const PriceLevel* lvl = best_level(book_side); lvl != nullptr;
             lvl = next_level_toward_mid(book_side, lvl->price)) {
            if (!crosses(side, limit, lvl->price)) break;
            total += lvl->total_qty;
        }
        return total;
    }

    // Iterate occupied levels of `s` from the top of book inward. Returns the
    // next occupied level strictly worse than `from_price`, or nullptr.
    [[nodiscard]] const PriceLevel* next_level_toward_mid(Side s, Price from_price) const noexcept {
        if (from_price == kNoPrice) return nullptr;
        const int si = side_idx(s);
        std::int64_t i = static_cast<std::int64_t>(index(from_price));
        if (s == Side::Buy) {
            for (--i; i >= 0; --i)
                if (test_bit(si, static_cast<std::size_t>(i)))
                    return &levels_[si][static_cast<std::size_t>(i)];
        } else {
            for (++i; i < static_cast<std::int64_t>(span_); ++i)
                if (test_bit(si, static_cast<std::size_t>(i)))
                    return &levels_[si][static_cast<std::size_t>(i)];
        }
        return nullptr;
    }

    // Depth snapshot: up to `max_levels` price points per side, best first.
    using DepthEntry = lob::DepthEntry;
    void snapshot(Side s, std::size_t max_levels, std::vector<DepthEntry>& out) const {
        out.clear();
        const PriceLevel* lvl = best_level(s);
        while (lvl != nullptr && out.size() < max_levels) {
            out.push_back({lvl->price, lvl->total_qty, lvl->count});
            lvl = next_level_toward_mid(s, lvl->price);
        }
    }

private:
    static constexpr int side_idx(Side s) noexcept { return static_cast<int>(s); }

    [[nodiscard]] std::size_t index(Price p) const noexcept {
        return static_cast<std::size_t>(p - min_price_);
    }

    void set_bit(int s, std::size_t i) noexcept {
        words_[s][i >> 6] |= (1ULL << (i & 63));
        summary_[s][(i >> 6) >> 6] |= (1ULL << ((i >> 6) & 63));
    }
    void clear_bit(int s, std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        words_[s][w] &= ~(1ULL << (i & 63));
        if (words_[s][w] == 0ULL)
            summary_[s][w >> 6] &= ~(1ULL << (w & 63));
    }
    [[nodiscard]] bool test_bit(int s, std::size_t i) const noexcept {
        return (words_[s][i >> 6] >> (i & 63)) & 1ULL;
    }

    [[nodiscard]] std::int64_t highest_set(int s) const noexcept {
        const auto& sum = summary_[s];
        for (std::size_t w = sum.size(); w-- > 0;) {
            if (sum[w] == 0ULL) continue;
            const std::size_t word = w * 64 + (63 - static_cast<std::size_t>(std::countl_zero(sum[w])));
            const std::uint64_t bits = words_[s][word];
            const std::size_t bit = 63 - static_cast<std::size_t>(std::countl_zero(bits));
            return static_cast<std::int64_t>(word * 64 + bit);
        }
        return -1;
    }
    [[nodiscard]] std::int64_t lowest_set(int s) const noexcept {
        const auto& sum = summary_[s];
        for (std::size_t w = 0; w < sum.size(); ++w) {
            if (sum[w] == 0ULL) continue;
            const std::size_t word = w * 64 + static_cast<std::size_t>(std::countr_zero(sum[w]));
            const std::uint64_t bits = words_[s][word];
            const std::size_t bit = static_cast<std::size_t>(std::countr_zero(bits));
            return static_cast<std::int64_t>(word * 64 + bit);
        }
        return -1;
    }

    Price min_price_;
    Price max_price_;
    std::size_t span_;

    std::vector<PriceLevel> levels_[2];
    std::vector<std::uint64_t> words_[2];
    std::vector<std::uint64_t> summary_[2];
    std::size_t order_count_[2] = {0, 0};
};

}  // namespace lob
