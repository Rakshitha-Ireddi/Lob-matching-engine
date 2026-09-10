// SPDX-License-Identifier: MIT
#pragma once

#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

// One price point in the book: a FIFO queue of orders plus cached aggregates.
struct PriceLevel {
    Price         price     = kNoPrice;
    Quantity      total_qty = 0;
    std::uint32_t count     = 0;
    Order*        head      = nullptr;  // oldest -> matched first (time priority)
    Order*        tail      = nullptr;  // newest

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail != nullptr) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        total_qty += o->remaining;
        ++count;
    }

    void unlink(Order* o) noexcept {
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        o->prev = o->next = nullptr;
        total_qty -= o->remaining;
        --count;
    }
};

}  // namespace lob
