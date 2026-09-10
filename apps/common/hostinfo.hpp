// SPDX-License-Identifier: MIT
#pragma once

#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
    #include <intrin.h>
#endif

// Best-effort host + CPU description for benchmark provenance.
inline std::string cpu_brand() {
#if defined(__x86_64__) || defined(_M_X64)
    int regs[4] = {0};
    char brand[49] = {0};
    auto cpuid = [&](int leaf) {
    #if defined(_MSC_VER)
        __cpuid(regs, leaf);
    #else
        __asm__ __volatile__("cpuid"
                             : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                             : "a"(leaf));
    #endif
    };
    cpuid(0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown x86-64";
    for (int i = 0; i < 3; ++i) {
        cpuid(0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string s(brand);
    // trim
    while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
#else
    return "non-x86 host";
#endif
}

inline std::string os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "unknown";
#endif
}

inline std::string compiler_id() {
#if defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

inline unsigned hw_threads() { return std::thread::hardware_concurrency(); }
