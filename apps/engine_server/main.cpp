// SPDX-License-Identifier: MIT
//
// engine_server -- runs the matching engine with a TCP order-entry gateway and
// a UDP market-data publisher, and (optionally) writes the dashboard snapshot.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "../common/args.hpp"
#include "../common/hostinfo.hpp"
#include "lob/engine_server.hpp"
#include "lob/net.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    if (args.has("help")) {
        std::printf(
            "usage: engine_server [options]\n"
            "  --host H            order-entry bind address (default 0.0.0.0)\n"
            "  --port P            order-entry TCP port     (default 9001)\n"
            "  --md-group G        market-data multicast    (default 239.7.7.7)\n"
            "  --md-port P         market-data UDP port     (default 9999)\n"
            "  --md-ttl N          multicast TTL            (default 1)\n"
            "  --band-min P        lowest tradable price    (default 1)\n"
            "  --band-max P        highest tradable price   (default 200000)\n"
            "  --pool N            resting-order capacity   (default 2000000)\n"
            "  --stats-file F      dashboard snapshot path  (default none)\n"
            "  --stats-interval-ms N                        (default 250)\n"
            "  --run-seconds N     exit after N seconds     (default: run until Ctrl-C)\n");
        return 0;
    }

    lob::net::startup();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    lob::ServerConfig cfg;
    cfg.tcp_host = args.str("host", "0.0.0.0");
    cfg.tcp_port = static_cast<std::uint16_t>(args.u64("port", 9001));
    cfg.md_group = args.str("md-group", "239.7.7.7");
    cfg.md_port = static_cast<std::uint16_t>(args.u64("md-port", 9999));
    cfg.md_ttl = static_cast<int>(args.i64("md-ttl", 1));
    cfg.engine.min_price = args.i64("band-min", 1);
    cfg.engine.max_price = args.i64("band-max", 200'000);
    cfg.engine.max_orders = static_cast<std::size_t>(args.u64("pool", 2'000'000));
    cfg.stats_file = args.str("stats-file", "");
    cfg.stats_interval_ms = static_cast<int>(args.i64("stats-interval-ms", 250));
    cfg.instrument = args.str("instrument", "LOB-PERP");
    cfg.engine_cpu = static_cast<int>(args.i64("pin-cpu", -1));

    lob::ServerPipeline server(cfg);
    server.start();

    std::printf("engine_server up\n");
    std::printf("  order entry : tcp://%s:%u\n", cfg.tcp_host.c_str(), cfg.tcp_port);
    std::printf("  market data : udp://%s:%u (ttl %d)\n", cfg.md_group.c_str(),
                cfg.md_port, cfg.md_ttl);
    std::printf("  price band  : [%lld, %lld]\n",
                static_cast<long long>(cfg.engine.min_price),
                static_cast<long long>(cfg.engine.max_price));
    if (!cfg.stats_file.empty())
        std::printf("  dashboard   : %s every %d ms\n", cfg.stats_file.c_str(),
                    cfg.stats_interval_ms);
    std::printf("  host        : %s\n", cpu_brand().c_str());
    std::fflush(stdout);

    const long run_seconds = static_cast<long>(args.i64("run-seconds", 0));
    const auto t0 = std::chrono::steady_clock::now();
    auto last_print = t0;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(2)) {
            last_print = now;
            const auto& c = server.counters();
            std::printf("[t+%4.0fs] cmds=%llu trades=%llu rejects=%llu exec=%llu md=%llu clients=%u drops=%llu\n",
                        std::chrono::duration<double>(now - t0).count(),
                        (unsigned long long)c.commands.load(),
                        (unsigned long long)c.trades.load(),
                        (unsigned long long)c.rejects.load(),
                        (unsigned long long)c.exec_sent.load(),
                        (unsigned long long)c.md_sent.load(),
                        c.clients.load(),
                        (unsigned long long)c.inbound_drops.load());
            std::fflush(stdout);
        }
        if (run_seconds > 0 &&
            std::chrono::duration<double>(now - t0).count() >= run_seconds) {
            break;
        }
    }

    std::printf("engine_server shutting down\n");
    server.stop();
    lob::net::shutdown();
    return 0;
}
