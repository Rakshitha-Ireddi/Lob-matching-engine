// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace lob {

// A minimal free-list pool allocator for fixed-size node containers
// (std::map, std::list, std::set): every single-object allocation is the same
// size, so one free list serves them all. Multi-object allocations fall back
// to ::operator new.
//
// Copies of the allocator share one pool (via shared_ptr), so a node
// allocated through one rebound copy can be freed through another --- which is
// what the standard requires of a container's allocator.
//
// Purpose: to test whether the throughput gap between the std::map order book
// and the bitset order book is an *allocator* effect (see
// docs/study-order-book-structures.md). Swapping std::allocator for this
// should close most of the gap if so.
template <class T>
class NodePoolAllocator {
public:
    using value_type = T;

    NodePoolAllocator() : state_(std::make_shared<State>()) {}

    template <class U>
    NodePoolAllocator(const NodePoolAllocator<U>& other) noexcept
        : state_(other.state_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n != 1) {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }
        State& s = *state_;
        if (s.free_head == nullptr) {
            grow(s);
        }
        void* p = s.free_head;
        s.free_head = *reinterpret_cast<void**>(p);
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p);
            return;
        }
        State& s = *state_;
        *reinterpret_cast<void**>(p) = s.free_head;
        s.free_head = p;
    }

    template <class U>
    bool operator==(const NodePoolAllocator<U>& o) const noexcept {
        return state_ == o.state_;
    }
    template <class U>
    bool operator!=(const NodePoolAllocator<U>& o) const noexcept {
        return !(*this == o);
    }

    template <class U>
    struct rebind {
        using other = NodePoolAllocator<U>;
    };

private:
    template <class U>
    friend class NodePoolAllocator;

    struct State {
        std::vector<std::unique_ptr<std::byte[]>> blocks;
        void* free_head = nullptr;
        static constexpr std::size_t kPerBlock = 8192;
    };

    static constexpr std::size_t slot_size() {
        return sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
    }

    static void grow(State& s) {
        // 16-byte default new alignment is enough for tree/list nodes.
        const std::size_t stride = (slot_size() + 15) & ~std::size_t{15};
        auto block = std::make_unique<std::byte[]>(stride * State::kPerBlock);
        std::byte* base = block.get();
        for (std::size_t i = State::kPerBlock; i-- > 0;) {
            void* slot = base + i * stride;
            *reinterpret_cast<void**>(slot) = s.free_head;
            s.free_head = slot;
        }
        s.blocks.push_back(std::move(block));
    }

    std::shared_ptr<State> state_;
};

}  // namespace lob
