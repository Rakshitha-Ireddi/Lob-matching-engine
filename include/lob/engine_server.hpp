// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lob/matching_engine.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/telemetry.hpp"
#include "lob/types.hpp"

namespace lob {

struct ServerConfig {
    std::string  tcp_host          = "127.0.0.1";
    std::uint16_t tcp_port         = 9001;
    std::string  md_group          = "239.7.7.7";
    std::uint16_t md_port          = 9999;
    int          md_ttl            = 1;

    EngineConfig engine{};

    std::size_t  inbound_ring      = 1u << 17;
    std::size_t  exec_ring         = 1u << 17;
    std::size_t  md_ring           = 1u << 17;

    std::string  stats_file        = "";     // dashboard snapshot ("" disables)
    int          stats_interval_ms = 250;
    std::string  instrument        = "LOB-PERP";
    int          tape_depth        = 30;     // trade prints kept for the tape
    int          ladder_depth      = 12;     // price levels per side in snapshot

    int          engine_cpu        = -1;     // >=0 pins the engine thread + gateway to cpu, cpu+1
};

// Three-stage pipeline wired by SPSC rings:
//
//   TCP gateway  --inbound-->  matching engine  --exec-->  TCP gateway (TX)
//                                    \--md--> UDP market-data publisher
//
// One thread per stage; the engine is the sole writer of the book.
// See docs/adr/0004-single-writer-engine.md and
// docs/adr/0010-thread-per-stage-pipeline.md.
class ServerPipeline {
public:
    explicit ServerPipeline(ServerConfig cfg);
    ~ServerPipeline();

    ServerPipeline(const ServerPipeline&) = delete;
    ServerPipeline& operator=(const ServerPipeline&) = delete;

    void start();
    void stop();

    struct Counters {
        std::atomic<std::uint64_t> commands{0};
        std::atomic<std::uint64_t> trades{0};
        std::atomic<std::uint64_t> rejects{0};
        std::atomic<std::uint64_t> exec_sent{0};
        std::atomic<std::uint64_t> md_sent{0};
        std::atomic<std::uint64_t> inbound_drops{0};
        std::atomic<std::uint32_t> clients{0};
    };
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

    // Latency of the engine stage (command receive -> last event emitted), ns.
    // Thread-safe copy.
    [[nodiscard]] LatencyHistogram engine_latency_snapshot() const;

    [[nodiscard]] std::uint16_t tcp_port() const noexcept { return cfg_.tcp_port; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ServerConfig cfg_;
    Counters counters_;
};

}  // namespace lob
