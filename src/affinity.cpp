// SPDX-License-Identifier: MIT
#include "lob/affinity.hpp"

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
    #include <cerrno>
    #include <cstring>
#endif

namespace lob {

PinResult pin_this_thread(int cpu) {
    PinResult r;
#if defined(_WIN32)
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << (cpu & 63);
    if (SetThreadAffinityMask(GetCurrentThread(), mask) != 0) {
        r.affinity_set = true;
    }
    if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        r.priority_raised =
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    }
    r.detail = "windows: affinity=" + std::to_string(r.affinity_set) +
               " high_priority=" + std::to_string(r.priority_raised);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    r.affinity_set =
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;

    sched_param sp{};
    sp.sched_priority = 80;
    r.priority_raised = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    r.detail = std::string("linux: affinity=") + (r.affinity_set ? "1" : "0") +
               " sched_fifo=" + (r.priority_raised ? "1" : "0") +
               (r.priority_raised ? "" : " (needs CAP_SYS_NICE)");
#else
    r.detail = "pinning not supported on this platform";
    (void)cpu;
#endif
    return r;
}

}  // namespace lob
