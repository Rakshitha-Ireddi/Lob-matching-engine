// SPDX-License-Identifier: MIT
#pragma once

#include <span>
#include <vector>

#include "lob/events.hpp"
#include "lob/matching_engine.hpp"

namespace lobtest {

// Collect every event a command produces (the engine's span is invalidated on
// the next call).
inline std::vector<lob::Event> run(lob::MatchingEngine& e, const lob::Command& c) {
    std::span<const lob::Event> s = e.process(c);
    return {s.begin(), s.end()};
}

inline int count(const std::vector<lob::Event>& evs, lob::EventType t) {
    int n = 0;
    for (const auto& e : evs)
        if (e.type == t) ++n;
    return n;
}

inline const lob::Event* first(const std::vector<lob::Event>& evs, lob::EventType t) {
    for (const auto& e : evs)
        if (e.type == t) return &e;
    return nullptr;
}

inline lob::EngineConfig small_book(lob::Price lo = 1, lob::Price hi = 10'000,
                                    std::size_t pool = 4096) {
    lob::EngineConfig c;
    c.min_price = lo;
    c.max_price = hi;
    c.max_orders = pool;
    return c;
}

inline lob::Command limit(lob::OrderId id, lob::Side s, lob::Price p, lob::Quantity q,
                          lob::ClientId c = 1) {
    return lob::Command::make_new(id, c, s, lob::OrderType::Limit, p, q);
}

}  // namespace lobtest
