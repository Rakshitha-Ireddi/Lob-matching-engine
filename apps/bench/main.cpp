// SPDX-License-Identifier: MIT
//
// lob_bench -- latency and throughput benchmark for the matching core.
//
//   core   : replay a synthetic order stream straight through
//            MatchingEngine::process(); measures command->events latency with
//            an overhead-corrected TSC and steady-state throughput.
//   e2e    : same stream pushed over a loopback TCP socket into the full
//            ServerPipeline; measures client wire round-trip.
//
// See docs/benchmarks.md for methodology and results.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../common/args.hpp"
#include "../common/hostinfo.hpp"
#include "lob/affinity.hpp"
#include "lob/clock.hpp"
#include "lob/engine_server.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_flow.hpp"
#include "lob/telemetry.hpp"
#include "lob/wire.hpp"
#include "lob/net.hpp"

namespace {

using lob::LatencyHistogram;

std::uint64_t tsc_overhead_cycles(const lob::TscClock& tsc) {
    std::uint64_t best = ~0ULL;
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t a = lob::TscClock::raw();
        const std::uint64_t b = lob::TscClock::raw();
        best = std::min(best, b - a);
    }
    (void)tsc;
    return best;
}

void print_row(const char* label, std::uint64_t ns) {
    if (ns >= 1000)
        std::printf("  %-22s %8.3f us\n", label, static_cast<double>(ns) / 1000.0);
    else
        std::printf("  %-22s %8llu ns\n", label, static_cast<unsigned long long>(ns));
}

int run_core(const Args& args) {
    const std::uint64_t events = args.u64("events", 5'000'000);
    const std::uint64_t warmup = args.u64("warmup", std::min<std::uint64_t>(events / 10, 1'000'000));
    const std::uint64_t seed = args.u64("seed", 42);
    const lob::Price band_min = args.i64("band-min", 1);
    const lob::Price band_max = args.i64("band-max", 200'000);
    const lob::Price mid = args.i64("mid", (band_min + band_max) / 2);

    lob::EngineConfig ecfg;
    ecfg.min_price = band_min;
    ecfg.max_price = band_max;
    ecfg.max_orders = static_cast<std::size_t>(args.u64("pool", 4'000'000));
    ecfg.emit_book_changed = !args.flag("no-book-events");

    lob::FlowConfig fcfg;
    fcfg.seed = seed;
    fcfg.ref_price = mid;
    fcfg.min_price = band_min;
    fcfg.max_price = band_max;
    fcfg.max_qty = static_cast<lob::Quantity>(args.u64("max-qty", 200));
    fcfg.p_new = args.f64("p-new", 0.55);
    fcfg.p_cancel = args.f64("p-cancel", 0.35);
    fcfg.p_market = args.f64("p-market", 0.04);
    fcfg.p_aggressive = args.f64("p-aggressive", 0.28);
    fcfg.depth_ticks = static_cast<int>(args.i64("depth-ticks", 48));
    fcfg.clients = static_cast<lob::ClientId>(args.u64("clients", 8));

    lob::MatchingEngine engine(ecfg);
    lob::OrderFlowGenerator gen(fcfg);

    std::string pin_detail = "not pinned";
    if (args.flag("pin")) {
        pin_detail = lob::pin_this_thread(static_cast<int>(args.i64("cpu", 2))).detail;
    }

    lob::TscClock tsc;
    const std::uint64_t ov = tsc_overhead_cycles(tsc);

    std::printf("lob_bench / core\n");
    std::printf("  host        : %s / %s\n", cpu_brand().c_str(), os_name().c_str());
    std::printf("  build       : %s, %s\n", compiler_id().c_str(),
#ifdef NDEBUG
                "Release"
#else
                "Debug"
#endif
    );
    std::printf("  tsc         : %.3f GHz%s, probe overhead %.1f ns\n", tsc.ghz(),
                tsc.calibrated() ? "" : " (uncalibrated)", tsc.to_nanos(ov) * 1.0);
    std::printf("  events      : %llu (warmup %llu)\n",
                static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(warmup));
    std::printf("  price band  : [%lld, %lld], mid %lld\n",
                static_cast<long long>(band_min), static_cast<long long>(band_max),
                static_cast<long long>(mid));
    std::printf("  scheduling  : %s\n", pin_detail.c_str());
    std::fflush(stdout);

    LatencyHistogram hist{60'000'000ULL, 3};
    const std::size_t chunk = 1u << 16;
    std::vector<lob::Command> batch(chunk);

    std::uint64_t done = 0;
    std::uint64_t checksum = 0;
    double engine_wall_s = 0.0;
    const auto t_start = std::chrono::steady_clock::now();

    while (done < events) {
        const std::size_t n =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk, events - done));
        for (std::size_t i = 0; i < n; ++i) batch[i] = gen.next();

        const auto cw0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t c0 = lob::TscClock::raw();
            const std::span<const lob::Event> evs = engine.process(batch[i]);
            const std::uint64_t c1 = lob::TscClock::raw();

            const bool measure = done + i >= warmup;
            if (measure) {
                std::uint64_t d = (c1 - c0 > ov) ? (c1 - c0 - ov) : 0;
                hist.record(tsc.to_nanos(d));
            }
            for (const auto& e : evs) checksum += e.price + e.quantity + static_cast<std::uint64_t>(e.type);
        }
        engine_wall_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - cw0).count();
        done += n;
    }
    const double total_wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    const auto& st = engine.stats();
    const double measured = static_cast<double>(hist.count());
    const double fill_ratio =
        st.new_orders ? static_cast<double>(st.trades) / static_cast<double>(st.new_orders) : 0.0;

    std::printf("\nthroughput\n");
    std::printf("  engine steady-state   %10.0f cmd/s  (%.2f ns/cmd)\n",
                static_cast<double>(events) / engine_wall_s,
                engine_wall_s * 1e9 / static_cast<double>(events));
    std::printf("  incl. flow-gen        %10.0f cmd/s\n",
                static_cast<double>(events) / total_wall_s);

    std::printf("\nlatency  (command -> all events emitted, n=%.0f)\n", measured);
    print_row("min", hist.min());
    print_row("p50", hist.percentile(50));
    print_row("p90", hist.percentile(90));
    print_row("p99", hist.percentile(99));
    print_row("p99.9", hist.percentile(99.9));
    print_row("p99.99", hist.percentile(99.99));
    print_row("max", hist.max());
    std::printf("  %-22s %8.1f ns\n", "mean", hist.mean());

    std::printf("\nwork done\n");
    std::printf("  trades %llu, shares %llu, rejects %llu, peak resting %llu\n",
                (unsigned long long)st.trades, (unsigned long long)st.shares_traded,
                (unsigned long long)st.rejects, (unsigned long long)st.peak_resting);
    std::printf("  fill ratio (trades/new) %.3f, resting now %zu\n", fill_ratio,
                engine.resting_orders());
    std::printf("  checksum %llu\n", (unsigned long long)checksum);

    lob::BenchResult r;
    r.mode = "core";
    r.host = os_name();
    r.cpu = cpu_brand();
    r.build =
#ifdef NDEBUG
        "Release";
#else
        "Debug";
#endif
    r.compiler = compiler_id();
    r.events = events;
    r.wall_s = engine_wall_s;
    r.throughput_ops = static_cast<double>(events) / engine_wall_s;
    r.trades = st.trades;
    r.shares = st.shares_traded;
    r.rejects = st.rejects;
    r.peak_resting = st.peak_resting;
    r.fill_ratio = fill_ratio;
    r.lat_scope = "engine command->events";
    r.lat_min = hist.min();
    r.lat_p50 = hist.percentile(50);
    r.lat_p90 = hist.percentile(90);
    r.lat_p99 = hist.percentile(99);
    r.lat_p999 = hist.percentile(99.9);
    r.lat_p9999 = hist.percentile(99.99);
    r.lat_max = hist.max();
    r.lat_mean = hist.mean();
    r.hist = hist.log_bins(10);   // compact, for the JSON document

    const std::string js = args.str("json", "");
    if (!js.empty()) {
        lob::write_json_file(js, lob::to_json(r));
        std::printf("\nwrote %s\n", js.c_str());
    }
    const std::string csv = args.str("csv", "");
    if (!csv.empty()) {
        lob::write_hist_csv(csv, hist.bins());   // full fidelity, for plotting
        std::printf("wrote %s\n", csv.c_str());
    }
    return 0;
}

int run_e2e(const Args& args) {
    lob::net::startup();

    const std::uint64_t events = args.u64("events", 500'000);
    const std::uint64_t warmup = args.u64("warmup", std::min<std::uint64_t>(events / 10, 50'000));
    const std::uint16_t port = static_cast<std::uint16_t>(args.u64("port", 9040));
    const lob::Price band_min = args.i64("band-min", 1);
    const lob::Price band_max = args.i64("band-max", 200'000);

    lob::ServerConfig scfg;
    scfg.tcp_host = "127.0.0.1";
    scfg.tcp_port = port;
    scfg.md_group = args.str("md-group", "239.7.7.7");
    scfg.md_port = static_cast<std::uint16_t>(args.u64("md-port", 9041));
    scfg.engine.min_price = band_min;
    scfg.engine.max_price = band_max;
    scfg.engine.max_orders = 2'000'000;
    scfg.stats_file = "";

    lob::ServerPipeline server(scfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    lob::net::Socket cli = lob::net::tcp_connect("127.0.0.1", port);
    if (!cli.valid()) {
        std::printf("e2e: could not connect to engine\n");
        server.stop();
        return 1;
    }
    lob::net::set_nonblocking(cli.get());

    lob::FlowConfig fcfg;
    fcfg.seed = args.u64("seed", 7);
    fcfg.ref_price = (band_min + band_max) / 2;
    fcfg.min_price = band_min;
    fcfg.max_price = band_max;
    fcfg.clients = 1;
    lob::OrderFlowGenerator gen(fcfg);

    LatencyHistogram rtt{2'000'000'000ULL, 3};  // headroom for loopback tail
    std::vector<lob::TsNanos> sent_at(events + 16, 0);
    std::vector<std::uint8_t> rx;
    rx.reserve(1 << 20);
    std::array<std::uint8_t, 64 * 1024> rbuf{};
    std::array<std::uint8_t, 128> tbuf{};

    std::printf("lob_bench / e2e  (loopback TCP round-trip)\n");
    std::printf("  host   : %s / %s\n", cpu_brand().c_str(), os_name().c_str());
    std::printf("  events : %llu (warmup %llu)\n", (unsigned long long)events,
                (unsigned long long)warmup);
    std::fflush(stdout);

    std::uint64_t sent = 0, acked = 0;
    const auto t0 = std::chrono::steady_clock::now();

    auto pump_rx = [&]() {
        for (;;) {
            long n = lob::net::tcp_recv(cli.get(), rbuf.data(), rbuf.size());
            if (n <= 0) break;
            rx.insert(rx.end(), rbuf.data(), rbuf.data() + n);
        }
        std::size_t off = 0;
        while (true) {
            std::span<const std::uint8_t> rest(rx.data() + off, rx.size() - off);
            auto hdr = lob::wire::peek_header(rest);
            if (!hdr) break;
            std::span<const std::uint8_t> frame(rx.data() + off, hdr->length);
            if (hdr->type == lob::wire::MsgType::ExecReport) {
                if (auto er = lob::wire::decode_exec_report(frame)) {
                    if (er->event == lob::EventType::Accepted && er->order_id <= events) {
                        const lob::TsNanos s = sent_at[er->order_id];
                        if (s != 0) {
                            const lob::TsNanos now = lob::now_ns();
                            if (acked >= warmup) rtt.record(now - s);
                            ++acked;
                        }
                    }
                }
            }
            off += hdr->length;
        }
        if (off) rx.erase(rx.begin(), rx.begin() + off);
    };

    while (acked < events) {
        if (sent < events && sent - acked < 2000) {
            lob::Command c = gen.next();
            // force NEW orders with monotone ids so RTT pairing is trivial
            lob::wire::NewOrder m{};
            m.order_id = sent + 1;
            m.client_id = 1;
            m.side = (c.side);
            m.type = lob::OrderType::Limit;
            m.price = c.price == lob::kNoPrice
                          ? (m.side == lob::Side::Buy ? fcfg.ref_price - 5 : fcfg.ref_price + 5)
                          : c.price;
            m.quantity = c.quantity ? c.quantity : 1;
            const lob::TsNanos ts = lob::now_ns();
            m.ts_client = ts;
            sent_at[m.order_id] = ts;
            std::size_t k = lob::wire::encode(tbuf.data(), tbuf.size(), 0, m);
            long w = lob::net::tcp_send(cli.get(), tbuf.data(), k);
            if (w == static_cast<long>(k)) ++sent;
        }
        pump_rx();
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
            std::printf("e2e: timed out (sent %llu acked %llu)\n",
                        (unsigned long long)sent, (unsigned long long)acked);
            break;
        }
    }

    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LatencyHistogram eng = server.engine_latency_snapshot();

    std::printf("\nclient wire round-trip  (send -> Accepted received, n=%llu)\n",
                (unsigned long long)rtt.count());
    print_row("min", rtt.min());
    print_row("p50", rtt.percentile(50));
    print_row("p99", rtt.percentile(99));
    print_row("p99.9", rtt.percentile(99.9));
    print_row("max", rtt.max());
    std::printf("\nengine stage latency (in-process, command->events)\n");
    print_row("p50", eng.percentile(50));
    print_row("p99", eng.percentile(99));
    std::printf("\nsustained %.0f new-orders/s over %.2fs\n",
                static_cast<double>(acked) / wall, wall);

    const std::string js = args.str("json", "");
    if (!js.empty()) {
        lob::BenchResult r;
        r.mode = "e2e";
        r.host = os_name();
        r.cpu = cpu_brand();
        r.compiler = compiler_id();
        r.events = rtt.count();
        r.wall_s = wall;
        r.throughput_ops = static_cast<double>(acked) / wall;
        r.lat_scope = "client wire round-trip";
        r.lat_min = rtt.min();
        r.lat_p50 = rtt.percentile(50);
        r.lat_p90 = rtt.percentile(90);
        r.lat_p99 = rtt.percentile(99);
        r.lat_p999 = rtt.percentile(99.9);
        r.lat_p9999 = rtt.percentile(99.99);
        r.lat_max = rtt.max();
        r.lat_mean = rtt.mean();
        r.hist = rtt.log_bins(10);
        lob::write_json_file(js, lob::to_json(r));
        std::printf("wrote %s\n", js.c_str());
    }

    server.stop();
    lob::net::shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string mode = args.str("mode", "core");
    if (args.has("help")) {
        std::printf(
            "usage: lob_bench --mode core|e2e [options]\n"
            "  core: --events N --warmup N --seed S --band-min P --band-max P --mid P\n"
            "        --pool N --clients N --p-new f --p-cancel f --p-market f --p-aggressive f\n"
            "        --json file --csv file --no-book-events\n"
            "  e2e : --events N --port P --md-port P --seed S --json file\n");
        return 0;
    }
    if (mode == "e2e") return run_e2e(args);
    return run_core(args);
}
