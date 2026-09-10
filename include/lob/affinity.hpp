// SPDX-License-Identifier: MIT
#pragma once

#include <string>

// Best-effort CPU pinning and scheduling priority for the latency-sensitive
// threads. Real deployments pin to isolated cores (isolcpus / cpuset) and run
// SCHED_FIFO; here we do what is possible without root / without a tuned
// kernel and report what actually took effect.
namespace lob {

struct PinResult {
    bool affinity_set = false;
    bool priority_raised = false;
    std::string detail;
};

// Pin the *calling* thread to logical CPU `cpu` and raise its priority.
PinResult pin_this_thread(int cpu);

}  // namespace lob
