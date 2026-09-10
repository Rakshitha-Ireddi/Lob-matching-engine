// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <vector>

namespace lob {

// Fixed-capacity free-list pool.
//
// All storage is reserved once at construction; acquire()/release() are O(1)
// pointer pushes and never touch the global allocator, so the matching hot
// path has bounded, jitter-free memory behaviour.
template <class T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        free_.reserve(capacity);
        for (std::size_t i = capacity; i-- > 0;) {
            free_.push_back(&storage_[i]);
        }
    }

    [[nodiscard]] T* acquire() noexcept {
        if (free_.empty()) return nullptr;
        T* p = free_.back();
        free_.pop_back();
        ++in_use_;
        return p;
    }

    void release(T* p) noexcept {
        if (p == nullptr) return;
        p->reset();
        free_.push_back(p);
        --in_use_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }

    void tick_high_water() noexcept {
        if (in_use_ > high_water_) high_water_ = in_use_;
    }

private:
    std::vector<T> storage_;
    std::vector<T*> free_;
    std::size_t in_use_ = 0;
    std::size_t high_water_ = 0;
};

}  // namespace lob
