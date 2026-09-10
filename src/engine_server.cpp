// SPDX-License-Identifier: MIT
#include "lob/engine_server.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#include "lob/affinity.hpp"
#include "lob/clock.hpp"
#include "lob/net.hpp"
#include "lob/spsc_ring.hpp"
#include "lob/wire.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <emmintrin.h>
    #define LOB_RELAX() _mm_pause()
#else
    #define LOB_RELAX() std::this_thread::yield()
#endif

namespace lob {
namespace {

using Clock = std::chrono::steady_clock;

double secs_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Internal market-data message carried on the engine->publisher ring.
struct MdMsg {
    std::uint8_t kind;  // 0 = trade, 1 = book change
    wire::MdTrade trade;
    wire::MdBookChange book;
};

std::uint8_t exec_flags(const Event& e) {
    return e.aggressor ? 0x01 : 0x00;
}

}  // namespace

struct ServerPipeline::Impl {
    explicit Impl(ServerConfig c, ServerPipeline::Counters& ctr)
        : cfg(std::move(c)),
          counters(ctr),
          engine(cfg.engine),
          inbound(cfg.inbound_ring),
          exec(cfg.exec_ring),
          md(cfg.md_ring) {}

    ServerConfig cfg;
    ServerPipeline::Counters& counters;

    MatchingEngine engine;              // engine thread only
    SpscRing<Command> inbound;          // gateway -> engine
    SpscRing<wire::ExecReport> exec;    // engine -> gateway
    SpscRing<MdMsg> md;                 // engine -> publisher

    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::thread gateway_t, engine_t, publisher_t, telemetry_t;

    Clock::time_point started{};

    mutable std::mutex snap_mu;
    DashboardSnapshot snap;             // filled by engine thread
    LatencyHistogram shared_hist{60'000'000ULL, 3};

    // ---- gateway --------------------------------------------------
    void run_gateway() {
        if (cfg.engine_cpu >= 0) pin_this_thread(cfg.engine_cpu + 1);
        net::Socket listener = net::tcp_listen(cfg.tcp_host, cfg.tcp_port);
        if (!listener.valid()) {
            counters.inbound_drops.fetch_add(1);
            return;
        }
        net::Poller poller;
        poller.add_read(listener.get());

        struct Conn {
            net::Socket sock;
            ClientId client = 0;
            std::vector<std::uint8_t> in;
            std::vector<std::uint8_t> out;
        };
        std::unordered_map<net::Fd, Conn> conns;
        std::unordered_map<ClientId, net::Fd> route;

        std::array<std::uint8_t, 64 * 1024> rbuf{};
        std::array<std::uint8_t, 128> ebuf{};

        auto flush = [&](Conn& c) {
            while (!c.out.empty()) {
                long n = net::tcp_send(c.sock.get(), c.out.data(), c.out.size());
                if (n <= 0) break;  // -2 would-block, -1 error handled on recv
                c.out.erase(c.out.begin(), c.out.begin() + n);
            }
        };

        // Busy-poll: the order-entry path never blocks on the poll. A
        // production build pins this to an isolated core -- see
        // docs/adr/0010-thread-per-stage-pipeline.md.
        while (!stop.load(std::memory_order_relaxed)) {
            auto ready = poller.wait(0);
            for (const auto& r : ready) {
                if (r.fd == listener.get()) {
                    for (;;) {
                        net::Socket cs = net::tcp_accept(listener.get());
                        if (!cs.valid()) break;
                        net::Fd fd = cs.get();
                        Conn c;
                        c.sock = std::move(cs);
                        c.in.reserve(64 * 1024);
                        conns.emplace(fd, std::move(c));
                        poller.add_read(fd);
                        counters.clients.fetch_add(1);
                    }
                    continue;
                }
                auto it = conns.find(r.fd);
                if (it == conns.end()) continue;
                Conn& c = it->second;

                bool closed = false;
                for (;;) {
                    long n = net::tcp_recv(c.sock.get(), rbuf.data(), rbuf.size());
                    if (n == -2) break;         // drained
                    if (n <= 0) { closed = true; break; }
                    c.in.insert(c.in.end(), rbuf.data(), rbuf.data() + n);
                }

                std::size_t off = 0;
                while (true) {
                    std::span<const std::uint8_t> rest(c.in.data() + off, c.in.size() - off);
                    auto hdr = wire::peek_header(rest);
                    if (!hdr) break;
                    std::span<const std::uint8_t> frame(c.in.data() + off, hdr->length);
                    const TsNanos ts = now_ns();
                    Command cmd{};
                    bool have = false;
                    switch (hdr->type) {
                        case wire::MsgType::NewOrder:
                            if (auto m = wire::decode_new_order(frame)) {
                                cmd = Command::make_new(m->order_id, m->client_id, m->side,
                                                        m->type, m->price, m->quantity, ts);
                                c.client = m->client_id;
                                have = true;
                            }
                            break;
                        case wire::MsgType::CancelOrder:
                            if (auto m = wire::decode_cancel(frame)) {
                                cmd = Command::make_cancel(m->order_id, m->client_id, ts);
                                c.client = m->client_id;
                                have = true;
                            }
                            break;
                        case wire::MsgType::ModifyOrder:
                            if (auto m = wire::decode_modify(frame)) {
                                cmd = Command::make_modify(m->order_id, m->client_id,
                                                           m->price, m->quantity, ts);
                                c.client = m->client_id;
                                have = true;
                            }
                            break;
                        default:
                            break;
                    }
                    off += hdr->length;
                    if (have) {
                        route[c.client] = r.fd;
                        int spins = 0;
                        while (!inbound.try_push(cmd)) {
                            if (++spins > 4096) {
                                counters.inbound_drops.fetch_add(1);
                                break;
                            }
                            LOB_RELAX();
                        }
                    }
                }
                if (off > 0) c.in.erase(c.in.begin(), c.in.begin() + off);

                if (closed) {
                    if (c.client != 0) route.erase(c.client);
                    poller.remove(r.fd);
                    conns.erase(it);
                    counters.clients.fetch_sub(1);
                }
            }

            // engine -> client execution reports
            wire::ExecReport er;
            while (exec.try_pop(er)) {
                auto rit = route.find(er.client_id);
                if (rit == route.end()) continue;
                auto cit = conns.find(rit->second);
                if (cit == conns.end()) continue;
                std::size_t n = wire::encode(ebuf.data(), ebuf.size(), 0, er);
                if (n == 0) continue;
                Conn& c = cit->second;
                c.out.insert(c.out.end(), ebuf.data(), ebuf.data() + n);
                counters.exec_sent.fetch_add(1);
            }
            for (auto& [fd, c] : conns) flush(c);
        }
    }

    // ---- engine --------------------------------------------------
    void run_engine() {
        if (cfg.engine_cpu >= 0) pin_this_thread(cfg.engine_cpu);
        std::deque<TradePrint> tape;
        Price last_px = kNoPrice;
        auto last_snap = Clock::now();
        const auto interval = std::chrono::milliseconds(cfg.stats_interval_ms);
        LatencyHistogram hist{60'000'000ULL, 3};

        Command cmd;
        while (!stop.load(std::memory_order_relaxed)) {
            if (inbound.try_pop(cmd)) {
                const TsNanos t0 = cmd.ts_recv;
                std::span<const Event> events = engine.process(cmd);
                const TsNanos t1 = now_ns();
                if (t1 > t0) hist.record(t1 - t0);
                counters.commands.fetch_add(1, std::memory_order_relaxed);

                for (const Event& e : events) {
                    switch (e.type) {
                        case EventType::Rejected:
                            counters.rejects.fetch_add(1, std::memory_order_relaxed);
                            push_exec(e);
                            break;
                        case EventType::Accepted:
                        case EventType::Fill:
                        case EventType::Canceled:
                        case EventType::Replaced:
                            push_exec(e);
                            break;
                        case EventType::Trade: {
                            counters.trades.fetch_add(1, std::memory_order_relaxed);
                            last_px = e.price;
                            tape.push_back({e.ts_out, e.price, e.quantity, e.side});
                            if (tape.size() > static_cast<std::size_t>(cfg.tape_depth))
                                tape.pop_front();
                            MdMsg m{};
                            m.kind = 0;
                            m.trade = {0, e.price, e.quantity, e.side, e.ts_out};
                            (void)md.try_push(m);
                            break;
                        }
                        case EventType::BookChanged: {
                            MdMsg m{};
                            m.kind = 1;
                            m.book = {0, e.bid_px, e.bid_qty, e.ask_px, e.ask_qty, e.ts_out};
                            (void)md.try_push(m);
                            break;
                        }
                    }
                }
            } else {
                LOB_RELAX();
            }

            if (Clock::now() - last_snap >= interval) {
                last_snap = Clock::now();
                publish_snapshot(tape, last_px, hist);
            }
        }
    }

    void push_exec(const Event& e) {
        wire::ExecReport er{};
        er.order_id = e.order_id;
        er.contra_id = e.contra_id;
        er.client_id = e.client;
        er.event = e.type;
        er.side = e.side;
        er.reason = e.reason;
        er.flags = exec_flags(e);
        er.price = e.price;
        er.quantity = e.quantity;
        er.leaves = e.leaves;
        er.ts_client = e.ts_in;   // gateway-stamped receive time, echoed
        er.ts_engine = e.ts_out;
        int spins = 0;
        while (!exec.try_push(er)) {
            if (++spins > 8192) break;
            LOB_RELAX();
        }
    }

    void publish_snapshot(const std::deque<TradePrint>& tape, Price last_px,
                          const LatencyHistogram& hist) {
        std::vector<OrderBook::DepthEntry> bids, asks;
        engine.book().snapshot(Side::Buy, static_cast<std::size_t>(cfg.ladder_depth), bids);
        engine.book().snapshot(Side::Sell, static_cast<std::size_t>(cfg.ladder_depth), asks);

        std::lock_guard<std::mutex> lk(snap_mu);
        shared_hist = hist;

        snap.instrument = cfg.instrument;
        snap.generated_ns = now_ns();
        snap.commands_total = counters.commands.load();
        snap.trades_total = counters.trades.load();
        snap.shares_total = engine.stats().shares_traded;
        snap.rejects_total = counters.rejects.load();
        snap.best_bid = engine.book().best_bid();
        snap.best_ask = engine.book().best_ask();
        snap.spread = engine.book().spread();
        snap.last_px = last_px;
        snap.resting_orders = engine.resting_orders();

        snap.bids.clear();
        for (const auto& d : bids) snap.bids.push_back({d.price, d.qty, d.orders});
        snap.asks.clear();
        for (const auto& d : asks) snap.asks.push_back({d.price, d.qty, d.orders});
        snap.tape.assign(tape.begin(), tape.end());

        snap.lat_scope = "engine";
        snap.lat_count = hist.count();
        snap.lat_min = hist.min();
        snap.lat_p50 = hist.percentile(50);
        snap.lat_p90 = hist.percentile(90);
        snap.lat_p99 = hist.percentile(99);
        snap.lat_p999 = hist.percentile(99.9);
        snap.lat_max = hist.max();
        snap.lat_mean = hist.mean();
        snap.lat_hist = hist.log_bins(8);
    }

    // ---- publisher ---------------------------------------------
    void run_publisher() {
        net::Socket sock = net::udp_publisher(cfg.md_group, cfg.md_port, cfg.md_ttl);
        std::array<std::uint8_t, 128> buf{};
        std::uint64_t seq = 0;
        auto last_hb = Clock::now();
        MdMsg m;
        std::uint32_t idle = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (md.try_pop(m)) {
                idle = 0;
                std::size_t n = 0;
                if (m.kind == 0) {
                    m.trade.stream_seq = ++seq;
                    n = wire::encode(buf.data(), buf.size(), static_cast<std::uint32_t>(seq), m.trade);
                } else {
                    m.book.stream_seq = ++seq;
                    n = wire::encode(buf.data(), buf.size(), static_cast<std::uint32_t>(seq), m.book);
                }
                if (n && sock.valid()) {
                    net::udp_send(sock.get(), cfg.md_group, cfg.md_port, buf.data(), n);
                    counters.md_sent.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (++idle < 256) {
                LOB_RELAX();
            } else {
                // nothing flowing: yield the core instead of burning it
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            if (Clock::now() - last_hb >= std::chrono::milliseconds(500)) {
                last_hb = Clock::now();
                wire::MdHeartbeat hb{++seq, now_ns()};
                std::size_t n = wire::encode(buf.data(), buf.size(),
                                             static_cast<std::uint32_t>(seq), hb);
                if (n && sock.valid())
                    net::udp_send(sock.get(), cfg.md_group, cfg.md_port, buf.data(), n);
            }
        }
    }

    // ---- telemetry -------------------------------------------
    void run_telemetry() {
        if (cfg.stats_file.empty()) return;
        std::uint64_t prev_cmd = 0, prev_md = 0;
        auto prev_t = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.stats_interval_ms));
            DashboardSnapshot s;
            {
                std::lock_guard<std::mutex> lk(snap_mu);
                s = snap;
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - prev_t).count();
            const std::uint64_t c = counters.commands.load();
            const std::uint64_t mdc = counters.md_sent.load();
            if (dt > 0) {
                s.throughput_ops = static_cast<double>(c - prev_cmd) / dt;
                s.md_msgs_per_s = static_cast<double>(mdc - prev_md) / dt;
                s.md_mbit_per_s = s.md_msgs_per_s * 56.0 * 8.0 / 1e6;
            }
            prev_cmd = c;
            prev_md = mdc;
            prev_t = now;
            s.uptime_s = secs_since(started);
            s.wall_unix_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            s.host = "";
            write_json_file(cfg.stats_file, to_json(s));
        }
    }
};

ServerPipeline::ServerPipeline(ServerConfig cfg) : cfg_(cfg) {
    impl_ = std::make_unique<Impl>(std::move(cfg), counters_);
}

ServerPipeline::~ServerPipeline() { stop(); }

void ServerPipeline::start() {
    if (impl_->running.exchange(true)) return;
    impl_->stop.store(false);
    impl_->started = Clock::now();
    impl_->engine_t = std::thread([this] { impl_->run_engine(); });
    impl_->gateway_t = std::thread([this] { impl_->run_gateway(); });
    impl_->publisher_t = std::thread([this] { impl_->run_publisher(); });
    impl_->telemetry_t = std::thread([this] { impl_->run_telemetry(); });
}

void ServerPipeline::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->stop.store(true);
    if (impl_->gateway_t.joinable()) impl_->gateway_t.join();
    if (impl_->engine_t.joinable()) impl_->engine_t.join();
    if (impl_->publisher_t.joinable()) impl_->publisher_t.join();
    if (impl_->telemetry_t.joinable()) impl_->telemetry_t.join();
}

LatencyHistogram ServerPipeline::engine_latency_snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->snap_mu);
    return impl_->shared_hist;
}

}  // namespace lob
