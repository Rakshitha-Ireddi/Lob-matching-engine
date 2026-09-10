// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>

#include "lob/types.hpp"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <intrin.h>
#endif

namespace lob {

// Monotonic wall clock in nanoseconds.
//
// Backed by clock_gettime(CLOCK_MONOTONIC) on Linux and
// QueryPerformanceCounter on Windows (both via std::chrono::steady_clock,
// which maps to those primitives). The absolute epoch is arbitrary; only
// differences are meaningful.
[[nodiscard]] inline TsNanos now_ns() noexcept {
    return static_cast<TsNanos>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Optional TSC-based timestamp for the very hot measurement points, where the
// ~20 ns overhead of steady_clock::now() would distort a sub-microsecond
// sample. Calibrated once against now_ns().
class TscClock {
public:
    TscClock();

    [[nodiscard]] static std::uint64_t raw() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    #if defined(_MSC_VER)
        return __rdtsc();
    #else
        std::uint32_t lo, hi;
        __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    #endif
#else
        return now_ns();
#endif
    }

    [[nodiscard]] TsNanos to_nanos(std::uint64_t cycles) const noexcept {
        return static_cast<TsNanos>(static_cast<double>(cycles) * ns_per_cycle_);
    }

    [[nodiscard]] double ghz() const noexcept { return 1.0 / ns_per_cycle_; }
    [[nodiscard]] bool calibrated() const noexcept { return calibrated_; }

private:
    double ns_per_cycle_ = 1.0;
    bool calibrated_ = false;
};

}  // namespace lob
