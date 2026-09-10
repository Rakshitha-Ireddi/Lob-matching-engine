// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

// Minimal open-addressing map from a non-zero 64-bit key to a pointer.
//
// Linear probing, power-of-two capacity, backward-shift deletion (no
// tombstones). Sized once up front so the order-index lookups in the hot
// path never rehash. Key 0 is reserved as the empty slot marker.
template <class V>
class FlatPtrMap {
public:
    explicit FlatPtrMap(std::size_t expected) {
        std::size_t cap = 16;
        while (cap * 3 < expected * 4) cap <<= 1;  // keep load factor < 0.75
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }

    void insert(std::uint64_t key, V value) noexcept {
        if ((size_ + 1) * 4 >= (mask_ + 1) * 3) grow();
        std::size_t i = hash(key) & mask_;
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) {
                slots_[i].value = value;
                return;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{key, value};
        ++size_;
    }

    [[nodiscard]] V find(std::uint64_t key) const noexcept {
        std::size_t i = hash(key) & mask_;
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) return slots_[i].value;
            i = (i + 1) & mask_;
        }
        return V{};
    }

    bool erase(std::uint64_t key) noexcept {
        std::size_t i = hash(key) & mask_;
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) {
                remove_at(i);
                --size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        std::uint64_t key = 0;
        V value = V{};
    };

    void grow() noexcept {
        std::vector<Slot> old = std::move(slots_);
        const std::size_t cap = (mask_ + 1) << 1;
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != 0) insert(s.key, s.value);
        }
    }

    static std::uint64_t hash(std::uint64_t x) noexcept {
        // splitmix64 finalizer -- cheap and good on dense sequential ids.
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    // Backward-shift deletion: pull each following entry back into the hole
    // while it sits farther from its ideal slot than the hole does.
    void remove_at(std::size_t hole) noexcept {
        std::size_t i = hole;
        while (true) {
            i = (i + 1) & mask_;
            if (slots_[i].key == 0) {
                slots_[hole] = Slot{};
                return;
            }
            const std::size_t ideal = hash(slots_[i].key) & mask_;
            if (probe_dist(ideal, hole) < probe_dist(ideal, i)) {
                slots_[hole] = slots_[i];
                hole = i;
            }
        }
    }

    [[nodiscard]] std::size_t probe_dist(std::size_t ideal, std::size_t slot) const noexcept {
        return (slot - ideal) & mask_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace lob
