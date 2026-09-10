// SPDX-License-Identifier: MIT
#include "lob/telemetry.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace lob {
namespace {

struct Json {
    std::ostringstream os;
    int indent = 0;
    bool need_comma = false;

    void pad() {
        os << '\n';
        for (int i = 0; i < indent; ++i) os << "  ";
    }
    void pre_value() {
        if (need_comma) os << ',';
        pad();
        need_comma = true;
    }
    void key(const char* k) {
        pre_value();
        os << '"' << k << "\": ";
        need_comma = false;
    }
    void begin_obj() {
        os << '{';
        ++indent;
        need_comma = false;
    }
    void end_obj() {
        --indent;
        pad();
        os << '}';
        need_comma = true;
    }
    void begin_arr() {
        os << '[';
        ++indent;
        need_comma = false;
    }
    void end_arr() {
        --indent;
        if (need_comma) pad();
        os << ']';
        need_comma = true;
    }
    void str(const char* k, const std::string& v) {
        key(k);
        os << '"' << v << '"';
        need_comma = true;
    }
    void num(const char* k, double v) {
        key(k);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        os << buf;
        need_comma = true;
    }
    void inum(const char* k, long long v) {
        key(k);
        os << v;
        need_comma = true;
    }
};

const char* side_str(Side s) { return s == Side::Buy ? "buy" : "sell"; }

long long px(Price p) { return p == kNoPrice ? 0 : static_cast<long long>(p); }

}  // namespace

std::string to_json(const DashboardSnapshot& s) {
    Json j;
    j.begin_obj();
    j.str("instrument", s.instrument);
    j.str("engine_build", s.engine_build);
    j.str("host", s.host);
    j.inum("generated_ns", static_cast<long long>(s.generated_ns));
    j.inum("wall_unix_ms", static_cast<long long>(s.wall_unix_ms));
    j.num("uptime_s", s.uptime_s);

    j.inum("commands_total", static_cast<long long>(s.commands_total));
    j.inum("trades_total", static_cast<long long>(s.trades_total));
    j.inum("shares_total", static_cast<long long>(s.shares_total));
    j.inum("rejects_total", static_cast<long long>(s.rejects_total));
    j.num("throughput_ops", s.throughput_ops);
    j.num("md_msgs_per_s", s.md_msgs_per_s);
    j.num("md_mbit_per_s", s.md_mbit_per_s);
    j.inum("md_seq_gaps", static_cast<long long>(s.md_seq_gaps));

    j.inum("best_bid", px(s.best_bid));
    j.inum("best_ask", px(s.best_ask));
    j.inum("spread", px(s.spread));
    j.inum("last_px", px(s.last_px));
    j.inum("resting_orders", static_cast<long long>(s.resting_orders));

    auto dump_ladder = [&](const char* k, const std::vector<DepthRow>& rows) {
        j.key(k);
        j.begin_arr();
        for (const auto& r : rows) {
            j.pre_value();
            j.os << "{ \"price\": " << px(r.price) << ", \"qty\": " << r.qty
                 << ", \"orders\": " << r.orders << " }";
        }
        j.end_arr();
    };
    dump_ladder("bids", s.bids);
    dump_ladder("asks", s.asks);

    j.key("tape");
    j.begin_arr();
    for (const auto& t : s.tape) {
        j.pre_value();
        j.os << "{ \"ts\": " << static_cast<long long>(t.ts)
             << ", \"price\": " << px(t.price) << ", \"qty\": " << t.qty
             << ", \"aggressor\": \"" << side_str(t.aggressor) << "\" }";
    }
    j.end_arr();

    j.key("latency");
    j.begin_obj();
    j.str("scope", s.lat_scope);
    j.inum("count", static_cast<long long>(s.lat_count));
    j.inum("min", static_cast<long long>(s.lat_min));
    j.inum("p50", static_cast<long long>(s.lat_p50));
    j.inum("p90", static_cast<long long>(s.lat_p90));
    j.inum("p99", static_cast<long long>(s.lat_p99));
    j.inum("p999", static_cast<long long>(s.lat_p999));
    j.inum("max", static_cast<long long>(s.lat_max));
    j.num("mean", s.lat_mean);
    j.key("histogram");
    j.begin_arr();
    for (const auto& b : s.lat_hist) {
        j.pre_value();
        j.os << "{ \"lo\": " << b.lo << ", \"hi\": " << b.hi
             << ", \"count\": " << b.count << " }";
    }
    j.end_arr();
    j.end_obj();

    j.end_obj();
    return j.os.str();
}

std::string to_json(const BenchResult& r) {
    Json j;
    j.begin_obj();
    j.str("mode", r.mode);
    j.str("host", r.host);
    j.str("cpu", r.cpu);
    j.str("build", r.build);
    j.str("compiler", r.compiler);
    j.inum("events", static_cast<long long>(r.events));
    j.num("wall_s", r.wall_s);
    j.num("throughput_ops", r.throughput_ops);
    j.inum("trades", static_cast<long long>(r.trades));
    j.inum("shares", static_cast<long long>(r.shares));
    j.inum("rejects", static_cast<long long>(r.rejects));
    j.inum("peak_resting", static_cast<long long>(r.peak_resting));
    j.num("fill_ratio", r.fill_ratio);
    j.key("latency_ns");
    j.begin_obj();
    j.str("scope", r.lat_scope);
    j.inum("min", static_cast<long long>(r.lat_min));
    j.inum("p50", static_cast<long long>(r.lat_p50));
    j.inum("p90", static_cast<long long>(r.lat_p90));
    j.inum("p99", static_cast<long long>(r.lat_p99));
    j.inum("p999", static_cast<long long>(r.lat_p999));
    j.inum("p9999", static_cast<long long>(r.lat_p9999));
    j.inum("max", static_cast<long long>(r.lat_max));
    j.num("mean", r.lat_mean);
    j.end_obj();
    j.key("histogram");
    j.begin_arr();
    for (const auto& b : r.hist) {
        j.pre_value();
        j.os << "{ \"lo\": " << b.lo << ", \"hi\": " << b.hi
             << ", \"count\": " << b.count << " }";
    }
    j.end_arr();
    j.end_obj();
    return j.os.str();
}

bool write_json_file(const std::string& path, const std::string& json) {
    // Write-then-rename so a dashboard poll never sees a half-written file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << json << '\n';
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool write_hist_csv(const std::string& path,
                    const std::vector<LatencyHistogram::Bin>& bins) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "lo_ns,hi_ns,count\n";
    for (const auto& b : bins) f << b.lo << ',' << b.hi << ',' << b.count << '\n';
    return true;
}

}  // namespace lob
