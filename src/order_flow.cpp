// SPDX-License-Identifier: MIT
#include "lob/order_flow.hpp"

#include <algorithm>

namespace lob {
namespace {

std::uint64_t splitmix64(std::uint64_t& x) {
    x += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

}  // namespace

OrderFlowGenerator::OrderFlowGenerator(const FlowConfig& cfg)
    : cfg_(cfg), mid_(cfg.ref_price) {
    std::uint64_t seed = cfg.seed;
    for (auto& v : s_) v = splitmix64(seed);
    live_.reserve(1 << 16);
}

void OrderFlowGenerator::reset() {
    std::uint64_t seed = cfg_.seed;
    for (auto& v : s_) v = splitmix64(seed);
    next_id_ = 1;
    mid_ = cfg_.ref_price;
    live_.clear();
}

// xoshiro256** -- fast, good statistical quality, tiny state.
std::uint64_t OrderFlowGenerator::next_u64() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double OrderFlowGenerator::unit() noexcept {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint64_t OrderFlowGenerator::range(std::uint64_t n) noexcept {
    return n == 0 ? 0 : next_u64() % n;
}

void OrderFlowGenerator::track(OrderId id, ClientId c) {
    live_.push_back({id, c});
}

bool OrderFlowGenerator::peek_random(Live& out) noexcept {
    if (live_.empty()) return false;
    out = live_[range(live_.size())];
    return true;
}

bool OrderFlowGenerator::untrack_random(Live& out) noexcept {
    if (live_.empty()) return false;
    const std::size_t i = range(live_.size());
    out = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    return true;
}

Command OrderFlowGenerator::make_new() noexcept {
    if (unit() < cfg_.drift_prob) {
        const Price step = static_cast<Price>(1 + range(3));
        mid_ += (unit() < 0.5) ? -step : step;
        const Price margin = cfg_.depth_ticks + 4;
        mid_ = std::clamp<Price>(mid_, cfg_.min_price + margin, cfg_.max_price - margin);
    }

    const Side side = (unit() < 0.5) ? Side::Buy : Side::Sell;
    const ClientId client = static_cast<ClientId>(range(cfg_.clients));
    const Quantity qty =
        cfg_.min_qty + range(cfg_.max_qty - cfg_.min_qty + 1);
    const OrderId id = next_id_++;

    if (unit() < cfg_.p_market) {
        return Command::make_new(id, client, side, OrderType::Market, kNoPrice, qty);
    }

    const Price off = static_cast<Price>(1 + range(static_cast<std::uint64_t>(cfg_.depth_ticks)));
    const bool aggressive = unit() < cfg_.p_aggressive;
    Price price;
    if (side == Side::Buy) {
        price = aggressive ? mid_ + off : mid_ - off;
    } else {
        price = aggressive ? mid_ - off : mid_ + off;
    }
    price = std::clamp<Price>(price, cfg_.min_price, cfg_.max_price);

    if (!aggressive) track(id, client);
    return Command::make_new(id, client, side, OrderType::Limit, price, qty);
}

Command OrderFlowGenerator::next() noexcept {
    const double u = unit();

    const bool over_depth = live_.size() >= cfg_.max_live;
    if (!over_depth && (u < cfg_.p_new || live_.empty())) {
        return make_new();
    }

    if (over_depth || u < cfg_.p_new + cfg_.p_cancel) {
        Live v{};
        if (untrack_random(v)) return Command::make_cancel(v.id, v.client);
        return make_new();
    }

    Live v{};
    if (peek_random(v)) {
        const Quantity new_qty =
            cfg_.min_qty + range(cfg_.max_qty - cfg_.min_qty + 1);
        Price new_price = kNoPrice;
        if (unit() < 0.5) {
            const Price off =
                static_cast<Price>(1 + range(static_cast<std::uint64_t>(cfg_.depth_ticks)));
            new_price = std::clamp<Price>(
                (unit() < 0.5) ? mid_ - off : mid_ + off, cfg_.min_price, cfg_.max_price);
        }
        return Command::make_modify(v.id, v.client, new_price, new_qty);
    }
    return make_new();
}

}  // namespace lob
