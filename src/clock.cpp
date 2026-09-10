// SPDX-License-Identifier: MIT
#include "lob/clock.hpp"

#include <thread>

namespace lob {

TscClock::TscClock() {
#if defined(__x86_64__) || defined(_M_X64)
    // Calibrate cycles-per-nanosecond against the monotonic clock over a
    // short busy interval. Good enough for converting latency deltas; not a
    // substitute for an invariant-TSC frequency read.
    const TsNanos t0 = now_ns();
    const std::uint64_t c0 = raw();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // busy tail to reduce sleep-wakeup slop
    while (now_ns() - t0 < 25'000'000ULL) {
    }
    const std::uint64_t c1 = raw();
    const TsNanos t1 = now_ns();

    const double cycles = static_cast<double>(c1 - c0);
    const double nanos = static_cast<double>(t1 - t0);
    if (cycles > 0.0 && nanos > 0.0) {
        ns_per_cycle_ = nanos / cycles;
        calibrated_ = true;
    }
#endif
}

}  // namespace lob
