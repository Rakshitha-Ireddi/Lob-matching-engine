// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>

namespace lob {

// Fixed at 64: correct for every current x86-64 and Apple-silicon target, and
// keeping it a literal avoids the cross-TU ABI warning that comes with
// std::hardware_destructive_interference_size.
inline constexpr std::size_t kCacheLine = 64;

// Bounded wait-free single-producer / single-consumer queue.
//
// Capacity is rounded up to a power of two. Head and tail live on separate
// cache lines so the producer and consumer never fight over the same line.
// See docs/adr/0006-spsc-ring-ipc.md.
template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t min_capacity) {
        std::size_t cap = 2;
        while (cap < min_capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // Producer side.
    [[nodiscard]] bool try_push(const T& v) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (next == tail_cache_) return false;  // full
        }
        buf_[head] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) return false;  // empty
        }
        out = buf_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

private:
    std::vector<T> buf_;
    std::size_t mask_ = 0;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;  // producer's view of tail

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;  // consumer's view of head
};

}  // namespace lob
