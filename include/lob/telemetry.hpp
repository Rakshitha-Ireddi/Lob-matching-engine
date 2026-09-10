// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lob/latency_histogram.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// One row of the depth ladder for the dashboard snapshot.
struct DepthRow {
    Price         price;
    Quantity      qty;
    std::uint32_t orders;
};

struct TradePrint {
    TsNanos  ts;
    Price    price;
    Quantity qty;
    Side     aggressor;
};

// Everything the web dashboard renders, serialised to a single JSON file that
// the engine rewrites a few times a second.
struct DashboardSnapshot {
    std::string  instrument   = "LOB-PERP";
    std::string  engine_build = "";
    std::string  host         = "";
    TsNanos      generated_ns  = 0;
    std::uint64_t wall_unix_ms = 0;
    double       uptime_s      = 0.0;

    // throughput
    std::uint64_t commands_total   = 0;
    std::uint64_t trades_total     = 0;
    std::uint64_t shares_total     = 0;
    std::uint64_t rejects_total    = 0;
    double        throughput_ops   = 0.0;   // commands/sec, recent window
    double        md_msgs_per_s    = 0.0;
    double        md_mbit_per_s    = 0.0;
    std::uint64_t md_seq_gaps      = 0;

    // book
    Price    best_bid = kNoPrice;
    Price    best_ask = kNoPrice;
    Price    spread   = kNoPrice;
    Price    last_px  = kNoPrice;
    std::uint64_t resting_orders = 0;
    std::vector<DepthRow> bids;
    std::vector<DepthRow> asks;
    std::vector<TradePrint> tape;

    // latency (nanoseconds) -- engine command->event unless noted
    std::uint64_t lat_count = 0;
    std::uint64_t lat_min = 0, lat_p50 = 0, lat_p90 = 0, lat_p99 = 0,
                  lat_p999 = 0, lat_max = 0;
    double        lat_mean = 0.0;
    std::string   lat_scope = "engine";  // "engine" or "wire-roundtrip"
    std::vector<LatencyHistogram::Bin> lat_hist;
};

// Serialise to pretty JSON (no external dependency).
std::string to_json(const DashboardSnapshot&);
bool write_json_file(const std::string& path, const std::string& json);

// Benchmark result document.
struct BenchResult {
    std::string mode;
    std::string host;
    std::string cpu;
    std::string build;
    std::string compiler;
    std::uint64_t events = 0;
    double wall_s = 0.0;
    double throughput_ops = 0.0;
    std::uint64_t trades = 0;
    std::uint64_t shares = 0;
    std::uint64_t rejects = 0;
    std::uint64_t peak_resting = 0;
    double fill_ratio = 0.0;
    std::string lat_scope;
    std::uint64_t lat_min = 0, lat_p50 = 0, lat_p90 = 0, lat_p99 = 0,
                  lat_p999 = 0, lat_p9999 = 0, lat_max = 0;
    double lat_mean = 0.0;
    std::vector<LatencyHistogram::Bin> hist;
};

std::string to_json(const BenchResult&);
bool write_hist_csv(const std::string& path, const std::vector<LatencyHistogram::Bin>&);

}  // namespace lob
