// SPDX-License-Identifier: MIT
//
// md_simulator -- two roles used to exercise the engine end to end:
//
//   flow      : connect over TCP and generate a synthetic order stream at a
//               target rate, measuring order-entry round-trip latency.
//   subscribe : join the UDP market-data group, decode the feed and report
//               message rate, bandwidth and sequence gaps.

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
#include "lob/affinity.hpp"
#include "lob/clock.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/net.hpp"
#include "lob/order_flow.hpp"
#include "lob/telemetry.hpp"
#include "lob/wire.hpp"

namespace {

using Clock = std::chrono::steady_clock;

int run_flow(const Args& args) {
    lob::net::startup();
    const std::string host = args.str("host", "127.0.0.1");
    const std::uint16_t port = static_cast<std::uint16_t>(args.u64("port", 9001));
    const double rate = args.f64("rate", 100'000.0);
    const double duration = args.f64("duration", 20.0);
    const lob::Price band_min = args.i64("band-min", 1);
    const lob::Price band_max = args.i64("band-max", 200'000);

    if (args.flag("pin")) {
        lob::pin_this_thread(static_cast<int>(args.i64("cpu", 0)));
    }

    lob::net::Socket cli = lob::net::tcp_connect(host, port);
    if (!cli.valid()) {
        std::printf("flow: cannot connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    lob::net::set_nonblocking(cli.get());

    lob::FlowConfig fcfg;
    fcfg.seed = args.u64("seed", 42);
    fcfg.ref_price = (band_min + band_max) / 2;
    fcfg.min_price = band_min;
    fcfg.max_price = band_max;
    fcfg.clients = static_cast<lob::ClientId>(args.u64("clients", 4));
    fcfg.p_aggressive = args.f64("p-aggressive", fcfg.p_aggressive);
    fcfg.depth_ticks = static_cast<int>(args.i64("depth-ticks", fcfg.depth_ticks));
    lob::OrderFlowGenerator gen(fcfg);

    lob::LatencyHistogram rtt{2'000'000'000ULL, 3};
    std::vector<lob::TsNanos> sent_at;
    sent_at.assign(1, 0);

    std::array<std::uint8_t, 64 * 1024> rbuf{};
    std::array<std::uint8_t, 128> tbuf{};
    std::vector<std::uint8_t> rx;
    rx.reserve(1 << 20);

    std::uint64_t sent = 0, acks = 0, rejects = 0, fills = 0;
    const auto t0 = Clock::now();
    auto next_send = t0;
    const auto tick = std::chrono::duration<double>(1.0 / rate);

    std::printf("md_simulator / flow -> %s:%u  rate=%.0f/s duration=%.0fs\n",
                host.c_str(), port, rate, duration);
    std::fflush(stdout);

    auto pump = [&]() {
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
                    switch (er->event) {
                        case lob::EventType::Accepted:
                            if (er->order_id < sent_at.size() && sent_at[er->order_id]) {
                                rtt.record(lob::now_ns() - sent_at[er->order_id]);
                                ++acks;
                            }
                            break;
                        case lob::EventType::Fill: ++fills; break;
                        case lob::EventType::Rejected: ++rejects; break;
                        default: break;
                    }
                }
            }
            off += hdr->length;
        }
        if (off) rx.erase(rx.begin(), rx.begin() + off);
    };

    while (std::chrono::duration<double>(Clock::now() - t0).count() < duration) {
        const auto now = Clock::now();
        if (now - next_send > std::chrono::milliseconds(50)) {
            next_send = now;  // shed backlog instead of firehosing
        }
        if (now >= next_send) {
            next_send += std::chrono::duration_cast<Clock::duration>(tick);
            lob::Command c = gen.next();
            const lob::TsNanos ts = lob::now_ns();
            std::size_t k = 0;
            if (c.type == lob::CommandType::New) {
                lob::wire::NewOrder m{c.id, c.client, c.side, c.ord_type, c.price,
                                      c.quantity, ts};
                k = lob::wire::encode(tbuf.data(), tbuf.size(), 0, m);
                if (c.id >= sent_at.size()) sent_at.resize(c.id + 1, 0);
                sent_at[c.id] = ts;
            } else if (c.type == lob::CommandType::Cancel) {
                lob::wire::CancelOrder m{c.id, c.client, ts};
                k = lob::wire::encode(tbuf.data(), tbuf.size(), 0, m);
            } else {
                lob::wire::ModifyOrder m{c.id, c.client, c.price, c.quantity, ts};
                k = lob::wire::encode(tbuf.data(), tbuf.size(), 0, m);
            }
            if (k) {
                long w = lob::net::tcp_send(cli.get(), tbuf.data(), k);
                if (w == static_cast<long>(k)) ++sent;
            }
        }
        pump();
    }
    // drain a little
    for (int i = 0; i < 200; ++i) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("\nsent %llu cmds (%.0f/s), acks %llu, fills %llu, rejects %llu\n",
                (unsigned long long)sent, sent / wall, (unsigned long long)acks,
                (unsigned long long)fills, (unsigned long long)rejects);
    std::printf("order-entry round-trip latency (send -> Accepted, n=%llu)\n",
                (unsigned long long)rtt.count());
    std::printf("  p50 %.2f us  p99 %.2f us  p99.9 %.2f us  max %.2f us\n",
                rtt.percentile(50) / 1000.0, rtt.percentile(99) / 1000.0,
                rtt.percentile(99.9) / 1000.0, rtt.max() / 1000.0);

    const std::string js = args.str("json", "");
    if (!js.empty()) {
        lob::BenchResult r;
        r.mode = "flow";
        r.lat_scope = "order-entry round-trip";
        r.events = rtt.count();
        r.wall_s = wall;
        r.throughput_ops = sent / wall;
        r.lat_p50 = rtt.percentile(50);
        r.lat_p90 = rtt.percentile(90);
        r.lat_p99 = rtt.percentile(99);
        r.lat_p999 = rtt.percentile(99.9);
        r.lat_max = rtt.max();
        r.lat_mean = rtt.mean();
        r.hist = rtt.bins();
        lob::write_json_file(js, lob::to_json(r));
        std::printf("wrote %s\n", js.c_str());
    }

    lob::net::shutdown();
    return 0;
}

int run_subscribe(const Args& args) {
    lob::net::startup();
    const std::string group = args.str("md-group", "239.7.7.7");
    const std::uint16_t port = static_cast<std::uint16_t>(args.u64("md-port", 9999));
    const double duration = args.f64("duration", 20.0);

    lob::net::Socket sock = lob::net::udp_subscriber(group, port);
    if (!sock.valid()) {
        std::printf("subscribe: cannot join %s:%u\n", group.c_str(), port);
        return 1;
    }

    std::printf("md_simulator / subscribe <- udp://%s:%u  duration=%.0fs\n",
                group.c_str(), port, duration);
    std::fflush(stdout);

    std::array<std::uint8_t, 2048> buf{};
    std::uint64_t msgs = 0, bytes = 0, trades = 0, book_changes = 0, gaps = 0;
    std::uint64_t expect = 0;
    lob::Price last_px = lob::kNoPrice;

    const auto t0 = Clock::now();
    auto win_t = t0;
    std::uint64_t win_msgs = 0, win_bytes = 0;

    while (std::chrono::duration<double>(Clock::now() - t0).count() < duration) {
        long n = lob::net::udp_recv(sock.get(), buf.data(), buf.size());
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        } else {
            std::span<const std::uint8_t> frame(buf.data(), static_cast<std::size_t>(n));
            auto hdr = lob::wire::peek_header(frame);
            if (hdr) {
                ++msgs;
                bytes += static_cast<std::uint64_t>(n);
                ++win_msgs;
                win_bytes += static_cast<std::uint64_t>(n);
                if (expect != 0 && hdr->seq > expect) gaps += hdr->seq - expect;
                expect = hdr->seq + 1;
                if (hdr->type == lob::wire::MsgType::MdTrade) {
                    if (auto m = lob::wire::decode_md_trade(frame)) {
                        ++trades;
                        last_px = m->price;
                    }
                } else if (hdr->type == lob::wire::MsgType::MdBookChange) {
                    ++book_changes;
                }
            }
        }
        const auto now = Clock::now();
        if (now - win_t >= std::chrono::seconds(2)) {
            const double dt = std::chrono::duration<double>(now - win_t).count();
            std::printf("[t+%4.0fs] %8.0f msg/s  %7.2f Mbit/s  trades=%llu book=%llu gaps=%llu last=%lld\n",
                        std::chrono::duration<double>(now - t0).count(),
                        win_msgs / dt, win_bytes * 8.0 / 1e6 / dt,
                        (unsigned long long)trades, (unsigned long long)book_changes,
                        (unsigned long long)gaps,
                        last_px == lob::kNoPrice ? 0LL : static_cast<long long>(last_px));
            std::fflush(stdout);
            win_t = now;
            win_msgs = 0;
            win_bytes = 0;
        }
    }

    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("\ntotal %llu msgs, %.1f MB, %.0f msg/s avg, %llu trades, %llu book updates, %llu seq gaps\n",
                (unsigned long long)msgs, bytes / 1e6, msgs / wall,
                (unsigned long long)trades, (unsigned long long)book_changes,
                (unsigned long long)gaps);

    lob::net::shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string mode = args.str("mode", "flow");
    if (args.has("help")) {
        std::printf(
            "usage: md_simulator --mode flow|subscribe [options]\n"
            "  flow      : --host H --port P --rate N --duration S --seed S --clients N --json F\n"
            "  subscribe : --md-group G --md-port P --duration S\n");
        return 0;
    }
    if (mode == "subscribe") return run_subscribe(args);
    return run_flow(args);
}
