// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lob/flat_hash_map.hpp"

using namespace lob;

TEST(FlatPtrMap, InsertFindErase) {
    FlatPtrMap<int*> m(64);
    int a = 1, b = 2, c = 3;
    m.insert(10, &a);
    m.insert(20, &b);
    m.insert(30, &c);
    EXPECT_EQ(m.find(20), &b);
    EXPECT_EQ(m.size(), 3u);
    EXPECT_TRUE(m.erase(20));
    EXPECT_EQ(m.find(20), nullptr);
    EXPECT_FALSE(m.erase(20));
    EXPECT_EQ(m.find(10), &a);
    EXPECT_EQ(m.find(30), &c);
}

TEST(FlatPtrMap, OverwriteKeepsSingleEntry) {
    FlatPtrMap<int*> m(16);
    int a = 1, b = 2;
    m.insert(7, &a);
    m.insert(7, &b);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m.find(7), &b);
}

// Backward-shift deletion must not strand entries that probed past the hole.
TEST(FlatPtrMap, CollisionChainSurvivesDeletion) {
    FlatPtrMap<std::uintptr_t*> m(1024);
    std::mt19937_64 rng(12345);
    std::unordered_map<std::uint64_t, std::uintptr_t> ref;

    for (int i = 0; i < 4000; ++i) {
        std::uint64_t k = (rng() % 5000) + 1;
        int op = rng() % 3;
        if (op == 0) {
            auto* v = reinterpret_cast<std::uintptr_t*>(static_cast<std::uintptr_t>(k));
            m.insert(k, v);
            ref[k] = k;
        } else if (op == 1) {
            m.erase(k);
            ref.erase(k);
        } else {
            auto got = m.find(k);
            auto it = ref.find(k);
            if (it == ref.end()) {
                EXPECT_EQ(got, nullptr) << "key " << k;
            } else {
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(got), it->second) << "key " << k;
            }
        }
    }
    for (const auto& [k, v] : ref) {
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(m.find(k)), v) << "final key " << k;
    }
}
