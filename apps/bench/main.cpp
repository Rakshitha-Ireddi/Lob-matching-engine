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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "lob/map_order_book.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_flow.hpp"
#include "lob/sorted_vector_order_book.hpp"
#include "lob/telemetry.hpp"
#include "lob/wire.hpp"
#include "lob/net.hpp"

#ifdef LOB_HAS_LIQUIBOOK
    #include <book/order.h>
    #include <book/order_book.h>
#endif

namespace {

using lob::LatencyHistogram;

volatile std::uint64_t g_checksum_sink = 0;

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

struct CoreRun {
    std::string book;
    double engine_wall_s = 0.0;
    double total_wall_s = 0.0;
    lob::EngineStats stats{};
    std::size_t resting_now = 0;
    std::uint64_t events = 0;
    LatencyHistogram hist{60'000'000ULL, 3};
};

template <class Book>
CoreRun measure_core(const char* name, const lob::EngineConfig& ecfg,
                     const lob::FlowConfig& fcfg, std::uint64_t events,
                     std::uint64_t warmup, const lob::TscClock& tsc,
                     std::uint64_t ov, std::uint64_t* trade_hash = nullptr) {
    lob::BasicMatchingEngine<Book> engine(ecfg);
    lob::OrderFlowGenerator gen(fcfg);

    CoreRun run;
    run.book = name;
    run.events = events;

    const std::size_t chunk = 1u << 16;
    std::vector<lob::Command> batch(chunk);
    std::uint64_t done = 0;
    std::uint64_t checksum = 0;
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
            if (done + i >= warmup) {
                const std::uint64_t d = (c1 - c0 > ov) ? (c1 - c0 - ov) : 0;
                run.hist.record(tsc.to_nanos(d));
            }
            for (const auto& e : evs) {
                checksum += e.price + e.quantity + static_cast<std::uint64_t>(e.type);
                if (trade_hash && e.type == lob::EventType::Trade) {
                    std::uint64_t h = *trade_hash;
                    for (std::uint64_t v :
                         {static_cast<std::uint64_t>(e.price), e.quantity,
                          static_cast<std::uint64_t>(e.side == lob::Side::Buy ? 1 : 2)}) {
                        h ^= v;
                        h *= 1099511628211ULL;
                    }
                    *trade_hash = h;
                }
            }
        }
        run.engine_wall_s +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - cw0).count();
        done += n;
    }
    run.total_wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    run.stats = engine.stats();
    run.resting_now = engine.resting_orders();

    g_checksum_sink = checksum;  // keep the event walk from being elided
    return run;
}

// Book-only microbenchmark: replays the same flow but applies ONLY the order-book
// operations (add / remove / best_bid / best_ask / snapshot) -- no matching, no
// events. Isolates the data structure so a cache-behaviour tool (cachegrind,
// perf) attributes every difference to the book. See docs/study-order-book-structures.md.
template <class Book>
CoreRun measure_book(const char* name, const lob::EngineConfig& ec,
                     const lob::FlowConfig& fc, std::uint64_t events,
                     std::uint64_t warmup, const lob::TscClock& tsc,
                     std::uint64_t ov) {
    Book book(ec.min_price, ec.max_price);
    lob::OrderFlowGenerator gen(fc);

    CoreRun run;
    run.book = name;
    run.events = events;

    std::vector<lob::Order> arena(ec.max_orders);
    std::vector<lob::Order*> freelist;
    freelist.reserve(ec.max_orders);
    for (std::size_t i = arena.size(); i-- > 0;) freelist.push_back(&arena[i]);
    std::vector<lob::Order*> by_id;  // order id -> live Order*, or nullptr
    std::vector<lob::DepthEntry> depth;
    std::uint64_t sink = 0;

    const auto attach = [&](const lob::Command& c) -> lob::Order* {
        if (freelist.empty()) return nullptr;
        lob::Order* o = freelist.back();
        freelist.pop_back();
        *o = lob::Order{};
        o->id = c.id;
        o->side = c.side;
        o->price = c.price;
        o->quantity = o->remaining = (c.quantity ? c.quantity : 1);
        if (c.id >= by_id.size()) by_id.resize(c.id + 1, nullptr);
        by_id[c.id] = o;
        return o;
    };
    const auto detach = [&](lob::OrderId id) {
        freelist.push_back(by_id[id]);
        by_id[id] = nullptr;
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < events; ++i) {
        const lob::Command c = gen.next();
        const std::uint64_t c0 = lob::TscClock::raw();

        switch (c.type) {
            case lob::CommandType::New:
                if (c.price != lob::kNoPrice && book.in_band(c.price)) {
                    if (lob::Order* o = attach(c)) book.add(o);
                }
                break;
            case lob::CommandType::Cancel:
                if (c.id < by_id.size() && by_id[c.id]) {
                    book.remove(by_id[c.id]);
                    detach(c.id);
                }
                break;
            case lob::CommandType::Modify:
                if (c.id < by_id.size() && by_id[c.id]) {
                    lob::Order* o = by_id[c.id];
                    book.remove(o);
                    o->price = (c.price == lob::kNoPrice) ? o->price : c.price;
                    o->remaining = o->quantity = (c.quantity ? c.quantity : 1);
                    if (book.in_band(o->price)) {
                        book.add(o);
                    } else {
                        detach(c.id);
                    }
                }
                break;
        }

        sink ^= static_cast<std::uint64_t>(book.best_bid());
        sink ^= static_cast<std::uint64_t>(book.best_ask()) << 1;
        if ((i & 63) == 0) {
            book.snapshot(lob::Side::Buy, 10, depth);
            sink += depth.size();
        }

        const std::uint64_t c1 = lob::TscClock::raw();
        if (i >= warmup) {
            const std::uint64_t d = (c1 - c0 > ov) ? (c1 - c0 - ov) : 0;
            run.hist.record(tsc.to_nanos(d));
        }
    }
    run.engine_wall_s = run.total_wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    run.resting_now = book.order_count();
    g_checksum_sink = sink;
    return run;
}

// --- open-loop response-time model -------------------------------------------
//
// The `core` bench is closed-loop: it never issues the next command until the
// current one returns, so it measures *service time* and cannot observe
// queueing. `load` mode issues commands on a fixed wall-clock schedule at a
// target rate; each command's response time is measured from its *scheduled*
// arrival (not from when the engine got to it), so a backlog shows up as
// growing response time -- the classic coordinated-omission-free measurement.
struct LoadPoint {
    double offered_ops = 0;
    double achieved_ops = 0;
    std::uint64_t recorded = 0;
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    double mean = 0;
    bool sustained = true;   // engine kept up with the schedule
};

template <class Book>
LoadPoint measure_load(const lob::EngineConfig& ec, const lob::FlowConfig& fc,
                       double target_ops, std::uint64_t events, std::uint64_t warmup,
                       bool poisson, std::uint64_t seed) {
    lob::BasicMatchingEngine<Book> engine(ec);
    lob::OrderFlowGenerator gen(fc);

    std::vector<lob::Command> cmds(events);
    for (auto& c : cmds) c = gen.next();

    // warm-up: fill the book, untimed, off-schedule
    for (std::uint64_t i = 0; i < warmup && i < events; ++i) engine.process(cmds[i]);

    LatencyHistogram hist{2'000'000'000ULL, 3};
    const double gap_ns = 1e9 / target_ops;
    std::uint64_t rng = seed * 0x9e3779b97f4a7c15ULL + 1;
    const auto next_u = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (static_cast<double>(rng >> 11) + 1.0) * (1.0 / 9007199254740992.0);
    };

    const lob::TsNanos base = lob::now_ns();
    lob::TsNanos sched = base;
    std::uint64_t checksum = 0;
    lob::TsNanos last_done = base;

    for (std::uint64_t i = warmup; i < events; ++i) {
        sched += static_cast<lob::TsNanos>(poisson ? -std::log(next_u()) * gap_ns : gap_ns);
        lob::TsNanos t;
        while ((t = lob::now_ns()) < sched) { /* busy-wait to the schedule */
        }
        const std::span<const lob::Event> evs = engine.process(cmds[i]);
        last_done = lob::now_ns();
        hist.record(last_done - sched);
        for (const auto& e : evs) checksum += e.price + e.quantity;
    }
    g_checksum_sink = checksum;

    const std::uint64_t n = events - warmup;
    LoadPoint lp;
    lp.offered_ops = target_ops;
    lp.achieved_ops = static_cast<double>(n) * 1e9 / static_cast<double>(last_done - base);
    lp.recorded = hist.count();
    lp.p50 = hist.percentile(50);
    lp.p99 = hist.percentile(99);
    lp.p999 = hist.percentile(99.9);
    lp.p9999 = hist.percentile(99.99);
    lp.max = hist.max();
    lp.mean = hist.mean();
    // "sustained" = wall time within 20% of the ideal schedule
    lp.sustained = static_cast<double>(last_done - base) <
                   1.20 * static_cast<double>(n) * gap_ns;
    return lp;
}

int run_load(const Args& args) {
    const std::uint64_t events = args.u64("events", 3'000'000);
    const std::uint64_t warmup =
        args.u64("warmup", std::min<std::uint64_t>(events / 5, 500'000));
    const lob::Price band_min = args.i64("band-min", 1);
    const lob::Price band_max = args.i64("band-max", 200'000);
    const bool poisson = !args.flag("uniform");
    const std::uint64_t seed = args.u64("seed", 42);

    lob::EngineConfig ec;
    ec.min_price = band_min;
    ec.max_price = band_max;
    ec.max_orders = static_cast<std::size_t>(args.u64("pool", 4'000'000));

    lob::FlowConfig fc;
    fc.seed = seed;
    fc.ref_price = (band_min + band_max) / 2;
    fc.min_price = band_min;
    fc.max_price = band_max;
    fc.depth_ticks = static_cast<int>(args.i64("depth-ticks", 48));
    fc.clients = static_cast<lob::ClientId>(args.u64("clients", 8));

    std::string pin_detail = "not pinned";
    if (args.flag("pin"))
        pin_detail = lob::pin_this_thread(static_cast<int>(args.i64("cpu", 2))).detail;

    std::vector<double> rates;
    const std::string sweep = args.str("rate-sweep", "");
    if (!sweep.empty()) {
        std::size_t p = 0;
        while (p < sweep.size()) {
            std::size_t q = sweep.find(',', p);
            if (q == std::string::npos) q = sweep.size();
            rates.push_back(std::strtod(sweep.substr(p, q - p).c_str(), nullptr) * 1e6);
            p = q + 1;
        }
    } else {
        rates.push_back(args.f64("rate", 1'000'000.0));
    }

    std::printf("lob_bench / load  (open-loop response time)\n");
    std::printf("  host      : %s / %s\n", cpu_brand().c_str(), os_name().c_str());
    std::printf("  arrivals  : %s\n", poisson ? "Poisson" : "uniform");
    std::printf("  events    : %llu (warmup %llu), band [%lld,%lld], depth-ticks %d\n",
                (unsigned long long)events, (unsigned long long)warmup,
                (long long)band_min, (long long)band_max, fc.depth_ticks);
    std::printf("  scheduling: %s\n\n", pin_detail.c_str());

    std::printf("%10s %12s %6s %10s %10s %10s %10s\n", "offered", "achieved", "keep?",
                "p50", "p99", "p99.9", "max");
    std::vector<LoadPoint> pts;
    for (double r : rates) {
        LoadPoint lp = measure_load<lob::OrderBook>(ec, fc, r, events, warmup, poisson, seed);
        pts.push_back(lp);
        std::printf("%9.2fM %11.2fM %6s %9.2fus %9.2fus %9.2fus %9.2fus\n", r / 1e6,
                    lp.achieved_ops / 1e6, lp.sustained ? "yes" : "NO",
                    lp.p50 / 1e3, lp.p99 / 1e3, lp.p999 / 1e3, lp.max / 1e3);
    }

    const std::string js = args.str("json", "");
    if (!js.empty()) {
        std::string doc = "{\n  \"mode\": \"load\", \"arrivals\": \"" +
                          std::string(poisson ? "poisson" : "uniform") +
                          "\",\n  \"cpu\": \"" + cpu_brand() + "\", \"host\": \"" +
                          os_name() + "\", \"depth_ticks\": " +
                          std::to_string(fc.depth_ticks) + ",\n  \"points\": [\n";
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto& p = pts[i];
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "    { \"offered_ops\": %.0f, \"achieved_ops\": %.0f, "
                          "\"sustained\": %s, \"p50_ns\": %llu, \"p99_ns\": %llu, "
                          "\"p999_ns\": %llu, \"p9999_ns\": %llu, \"max_ns\": %llu, "
                          "\"mean_ns\": %.1f }%s\n",
                          p.offered_ops, p.achieved_ops, p.sustained ? "true" : "false",
                          (unsigned long long)p.p50, (unsigned long long)p.p99,
                          (unsigned long long)p.p999, (unsigned long long)p.p9999,
                          (unsigned long long)p.max, p.mean,
                          i + 1 < pts.size() ? "," : "");
            doc += buf;
        }
        doc += "  ]\n}\n";
        lob::write_json_file(js, doc);
        std::printf("\nwrote %s\n", js.c_str());
    }
    return 0;
}

#ifdef LOB_HAS_LIQUIBOOK
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
// Order type liquibook's OrderBook template needs. `remaining` is maintained
// from the fill callback so finished orders can be recycled to the arena.
struct LbOrder : public liquibook::book::Order {
    bool          buy = false;
    std::uint32_t id  = 0;
    liquibook::book::Price    px  = 0;
    liquibook::book::Quantity qty = 0;
    std::int64_t              remaining = 0;
    bool is_buy() const override { return buy; }
    liquibook::book::Price price() const override { return px; }
    liquibook::book::Quantity order_qty() const override { return qty; }
};

struct LbListener : public liquibook::book::OrderListener<LbOrder*> {
    std::uint64_t accepts = 0, rejects = 0, fills = 0, cancels = 0, shares = 0;
    std::vector<LbOrder*>* freelist = nullptr;
    std::vector<LbOrder*>* by_id = nullptr;
    std::uint64_t* trade_hash = nullptr;

    void retire(LbOrder* o) {
        if (o->id < by_id->size() && (*by_id)[o->id] == o) (*by_id)[o->id] = nullptr;
        freelist->push_back(o);
    }
    void on_accept(LbOrder* const&) override { ++accepts; }
    void on_reject(LbOrder* const&, const char*) override { ++rejects; }
    void on_fill(LbOrder* const& o, LbOrder* const& m, liquibook::book::Quantity q,
                 liquibook::book::Price price) override {
        ++fills;
        shares += q;
        if (trade_hash) {
            std::uint64_t h = *trade_hash;
            for (std::uint64_t v : {static_cast<std::uint64_t>(price), q,
                                    static_cast<std::uint64_t>(o->buy ? 1 : 2)}) {
                h ^= v;
                h *= 1099511628211ULL;
            }
            *trade_hash = h;
        }
        if ((o->remaining -= static_cast<std::int64_t>(q)) <= 0) retire(o);
        if ((m->remaining -= static_cast<std::int64_t>(q)) <= 0) retire(m);
    }
    void on_cancel(LbOrder* const& o) override {
        ++cancels;
        retire(o);
    }
    void on_cancel_reject(LbOrder* const&, const char*) override {}
    void on_replace(LbOrder* const&, const std::int64_t&, liquibook::book::Price) override {}
    void on_replace_reject(LbOrder* const&, const char*) override {}
    void on_trigger_stop(LbOrder* const&) override {}
};

// Replay the NEW + CANCEL subset of the flow through liquibook's per-order
// multimap book. Market orders and modifies are skipped (see the study doc),
// so pair this with a flow that has p_market = 0 and p_new + p_cancel = 1.
CoreRun measure_liquibook(const lob::EngineConfig& ec, const lob::FlowConfig& fc,
                          std::uint64_t events, std::uint64_t warmup,
                          const lob::TscClock& tsc, std::uint64_t ov,
                          std::uint64_t* trade_hash = nullptr) {
    liquibook::book::OrderBook<LbOrder*> book("BENCH");
    lob::OrderFlowGenerator gen(fc);

    CoreRun run;
    run.book = "liquibook";
    run.events = events;

    std::vector<LbOrder> arena(ec.max_orders);
    std::vector<LbOrder*> freelist;
    freelist.reserve(ec.max_orders);
    for (std::size_t i = arena.size(); i-- > 0;) freelist.push_back(&arena[i]);
    std::vector<LbOrder*> by_id;

    LbListener lst;
    lst.freelist = &freelist;
    lst.by_id = &by_id;
    lst.trade_hash = trade_hash;
    book.set_order_listener(&lst);

    std::uint64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < events; ++i) {
        const lob::Command c = gen.next();
        const std::uint64_t c0 = lob::TscClock::raw();

        if (c.type == lob::CommandType::New && c.price != lob::kNoPrice &&
            c.price > 0 && !freelist.empty()) {
            LbOrder* o = freelist.back();
            freelist.pop_back();
            o->buy = (c.side == lob::Side::Buy);
            o->id = static_cast<std::uint32_t>(c.id);
            o->px = static_cast<liquibook::book::Price>(c.price);
            o->qty = static_cast<liquibook::book::Quantity>(c.quantity ? c.quantity : 1);
            o->remaining = static_cast<std::int64_t>(o->qty);
            if (c.id >= by_id.size()) by_id.resize(c.id + 1, nullptr);
            by_id[c.id] = o;
            book.add(o, 0);
        } else if (c.type == lob::CommandType::Cancel && c.id < by_id.size() &&
                   by_id[c.id] != nullptr) {
            book.cancel(by_id[c.id]);
        }

        sink ^= book.bids().empty() ? 0 : book.bids().begin()->first.price();
        sink ^= book.asks().empty() ? 0 : (book.asks().begin()->first.price() << 1);

        const std::uint64_t c1 = lob::TscClock::raw();
        if (i >= warmup) {
            const std::uint64_t d = (c1 - c0 > ov) ? (c1 - c0 - ov) : 0;
            run.hist.record(tsc.to_nanos(d));
        }
    }
    run.engine_wall_s = run.total_wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    run.stats.trades = lst.fills;
    run.stats.shares_traded = lst.shares;
    run.stats.new_orders = lst.accepts;
    run.resting_now = book.bids().size() + book.asks().size();
    g_checksum_sink = sink;
    return run;
}
#pragma GCC diagnostic pop
#endif  // LOB_HAS_LIQUIBOOK

CoreRun dispatch_core(const std::string& book, bool book_only,
                      const lob::EngineConfig& ec, const lob::FlowConfig& fc,
                      std::uint64_t events, std::uint64_t warmup,
                      const lob::TscClock& tsc, std::uint64_t ov) {
    if (book_only) {
        if (book == "map")
            return measure_book<lob::MapOrderBook>("map", ec, fc, events, warmup, tsc, ov);
        if (book == "map-pooled")
            return measure_book<lob::PooledMapOrderBook>("map-pooled", ec, fc, events,
                                                         warmup, tsc, ov);
        if (book == "flat" || book == "vector" || book == "sorted-vector")
            return measure_book<lob::SortedVectorOrderBook>("flat", ec, fc, events, warmup,
                                                            tsc, ov);
        return measure_book<lob::OrderBook>("bitset", ec, fc, events, warmup, tsc, ov);
    }
    if (book == "map")
        return measure_core<lob::MapOrderBook>("map", ec, fc, events, warmup, tsc, ov);
    if (book == "map-pooled")
        return measure_core<lob::PooledMapOrderBook>("map-pooled", ec, fc, events, warmup,
                                                     tsc, ov);
    if (book == "flat" || book == "vector" || book == "sorted-vector")
        return measure_core<lob::SortedVectorOrderBook>("flat", ec, fc, events, warmup,
                                                        tsc, ov);
    return measure_core<lob::OrderBook>("bitset", ec, fc, events, warmup, tsc, ov);
}

void report_core(const CoreRun& run, bool book_only) {
    const char* unit = book_only ? "op/s" : "cmd/s";
    const char* scope = book_only ? "book op -> best-bid/ask updated"
                                  : "command -> all events emitted";
    std::printf("\n[%s] throughput\n", run.book.c_str());
    std::printf("  %-18s %12.0f %s  (%.2f ns each)\n",
                book_only ? "book ops" : "engine steady-state",
                static_cast<double>(run.events) / run.engine_wall_s, unit,
                run.engine_wall_s * 1e9 / static_cast<double>(run.events));

    std::printf("\n[%s] latency  (%s, n=%.0f)\n", run.book.c_str(), scope,
                static_cast<double>(run.hist.count()));
    print_row("min", run.hist.min());
    print_row("p50", run.hist.percentile(50));
    print_row("p90", run.hist.percentile(90));
    print_row("p99", run.hist.percentile(99));
    print_row("p99.9", run.hist.percentile(99.9));
    print_row("p99.99", run.hist.percentile(99.99));
    print_row("max", run.hist.max());
    std::printf("  %-22s %8.1f ns\n", "mean", run.hist.mean());

    if (!book_only) {
        const auto& st = run.stats;
        const double fill =
            st.new_orders
                ? static_cast<double>(st.trades) / static_cast<double>(st.new_orders)
                : 0.0;
        std::printf(
            "\n[%s] work: trades %llu, shares %llu, rejects %llu, fill %.3f, resting %zu\n",
            run.book.c_str(), (unsigned long long)st.trades,
            (unsigned long long)st.shares_traded, (unsigned long long)st.rejects, fill,
            run.resting_now);
    } else {
        std::printf("\n[%s] resting orders now %zu\n", run.book.c_str(), run.resting_now);
    }
}

lob::BenchResult to_bench_result(const CoreRun& run) {
    const auto& st = run.stats;
    lob::BenchResult r;
    r.mode = "core/" + run.book;
    r.host = os_name();
    r.cpu = cpu_brand();
    r.build =
#ifdef NDEBUG
        "Release";
#else
        "Debug";
#endif
    r.compiler = compiler_id();
    r.events = run.events;
    r.wall_s = run.engine_wall_s;
    r.throughput_ops = static_cast<double>(run.events) / run.engine_wall_s;
    r.trades = st.trades;
    r.shares = st.shares_traded;
    r.rejects = st.rejects;
    r.peak_resting = st.peak_resting;
    r.fill_ratio = st.new_orders ? static_cast<double>(st.trades) /
                                       static_cast<double>(st.new_orders)
                                 : 0.0;
    r.lat_scope = "engine command->events (" + run.book + " book)";
    r.lat_min = run.hist.min();
    r.lat_p50 = run.hist.percentile(50);
    r.lat_p90 = run.hist.percentile(90);
    r.lat_p99 = run.hist.percentile(99);
    r.lat_p999 = run.hist.percentile(99.9);
    r.lat_p9999 = run.hist.percentile(99.99);
    r.lat_max = run.hist.max();
    r.lat_mean = run.hist.mean();
    r.hist = run.hist.log_bins(10);
    return r;
}

int run_core(const Args& args, bool book_only) {
    const std::uint64_t events = args.u64("events", book_only ? 3'000'000 : 5'000'000);
    const std::uint64_t warmup =
        args.u64("warmup", std::min<std::uint64_t>(events / 10, 1'000'000));
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

    std::string pin_detail = "not pinned";
    if (args.flag("pin")) {
        pin_detail = lob::pin_this_thread(static_cast<int>(args.i64("cpu", 2))).detail;
    }

    lob::TscClock tsc;
    const std::uint64_t ov = tsc_overhead_cycles(tsc);

    std::printf("lob_bench / %s\n", book_only ? "book (data-structure only)" : "core");
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

    const std::string js = args.str("json", "");

#ifdef LOB_HAS_LIQUIBOOK
    if (args.flag("engine-compare")) {
        // engine vs engine on an identical NEW+CANCEL stream (no market / modify,
        // which liquibook's replay path here does not cover).
        lob::FlowConfig ef = fcfg;
        ef.p_market = 0.0;
        ef.p_new = 0.60;
        ef.p_cancel = 0.40;
        std::printf("\nengine vs engine: this bitset book vs OCI liquibook "
                    "(NEW+CANCEL stream, %llu events)\n",
                    static_cast<unsigned long long>(events));

        // parity: same trade stream (price, qty, aggressor side) from both engines
        {
            std::uint64_t h_mine = 1469598103934665603ULL, h_lb = 1469598103934665603ULL;
            const std::uint64_t vn = std::min<std::uint64_t>(events, 300'000);
            (void)measure_core<lob::OrderBook>("verify", ecfg, ef, vn, 0, tsc, ov, &h_mine);
            (void)measure_liquibook(ecfg, ef, vn, 0, tsc, ov, &h_lb);
            std::printf("  trade-stream parity over %llu events: %s (0x%llx)\n",
                        (unsigned long long)vn,
                        h_mine == h_lb ? "MATCH" : "MISMATCH",
                        (unsigned long long)h_mine);
        }

        CoreRun mine =
            measure_core<lob::OrderBook>("this/bitset", ecfg, ef, events, warmup, tsc, ov);
        report_core(mine, false);
        CoreRun lb = measure_liquibook(ecfg, ef, events, warmup, tsc, ov);
        report_core(lb, false);

        const double bt = static_cast<double>(mine.events) / mine.engine_wall_s;
        std::printf("\n%-14s %12s %10s %9s %9s %9s %8s\n", "engine", "cmd/s", "ns/cmd",
                    "p50", "p99", "p99.9", "vs this");
        for (const auto& r : {mine, lb}) {
            const double t = static_cast<double>(r.events) / r.engine_wall_s;
            std::printf("%-14s %12.0f %10.1f %9llu %9llu %9llu %7.2fx\n", r.book.c_str(), t,
                        r.engine_wall_s * 1e9 / static_cast<double>(r.events),
                        (unsigned long long)r.hist.percentile(50),
                        (unsigned long long)r.hist.percentile(99),
                        (unsigned long long)r.hist.percentile(99.9), bt / t);
        }
        std::printf("\ntrades: this %llu, liquibook %llu\n",
                    (unsigned long long)mine.stats.trades,
                    (unsigned long long)lb.stats.trades);

        if (!js.empty()) {
            std::string doc = "{\n  \"mode\": \"engine-compare\",\n  \"events\": " +
                              std::to_string(events) + ",\n  \"cpu\": \"" + cpu_brand() +
                              "\",\n  \"host\": \"" + os_name() + "\",\n  \"runs\": [\n    " +
                              lob::to_json(to_bench_result(mine)) + ",\n    " +
                              lob::to_json(to_bench_result(lb)) + "\n  ]\n}\n";
            lob::write_json_file(js, doc);
            std::printf("wrote %s\n", js.c_str());
        }
        return 0;
    }
    if (args.str("engine", "mine") == "liquibook") {
        lob::FlowConfig ef = fcfg;
        ef.p_market = 0.0;
        ef.p_new = 0.60;
        ef.p_cancel = 0.40;
        const CoreRun lb = measure_liquibook(ecfg, ef, events, warmup, tsc, ov);
        report_core(lb, false);
        if (!js.empty()) lob::write_json_file(js, lob::to_json(to_bench_result(lb)));
        return 0;
    }
#endif

    if (args.flag("compare")) {
        std::printf("\ncomparing order-book data structures (identical flow, %llu events)\n",
                    static_cast<unsigned long long>(events));
        const bool with_pooled = args.flag("with-pooled-map");
        std::vector<const char*> books = {"bitset", "map", "flat"};
        if (with_pooled) books.insert(books.begin() + 2, "map-pooled");
        std::vector<CoreRun> runs;
        for (const char* b : books) {
            std::fflush(stdout);
            runs.push_back(dispatch_core(b, book_only, ecfg, fcfg, events, warmup, tsc, ov));
            report_core(runs.back(), book_only);
        }

        const double base_tput =
            static_cast<double>(runs[0].events) / runs[0].engine_wall_s;
        std::printf(
            "\n%-11s %12s %10s %9s %9s %9s %9s %8s\n", "book", "cmd/s", "ns/cmd",
            "p50", "p90", "p99", "p99.9", "vs bit");
        for (const auto& r : runs) {
            const double tput = static_cast<double>(r.events) / r.engine_wall_s;
            std::printf("%-11s %12.0f %10.1f %9llu %9llu %9llu %9llu %7.2fx\n",
                        r.book.c_str(), tput,
                        r.engine_wall_s * 1e9 / static_cast<double>(r.events),
                        (unsigned long long)r.hist.percentile(50),
                        (unsigned long long)r.hist.percentile(90),
                        (unsigned long long)r.hist.percentile(99),
                        (unsigned long long)r.hist.percentile(99.9),
                        base_tput / tput);
        }

        if (!js.empty()) {
            std::string doc = "{\n  \"mode\": \"core-compare\",\n  \"events\": " +
                              std::to_string(events) + ",\n  \"cpu\": \"" + cpu_brand() +
                              "\",\n  \"host\": \"" + os_name() + "\",\n  \"runs\": [\n";
            for (std::size_t i = 0; i < runs.size(); ++i) {
                doc += "    " + lob::to_json(to_bench_result(runs[i]));
                doc += (i + 1 < runs.size()) ? ",\n" : "\n";
            }
            doc += "  ]\n}\n";
            lob::write_json_file(js, doc);
            std::printf("\nwrote %s\n", js.c_str());
        }
        return 0;
    }

    const std::string book = args.str("book", "bitset");
    const CoreRun run = dispatch_core(book, book_only, ecfg, fcfg, events, warmup, tsc, ov);
    report_core(run, book_only);

    if (!js.empty()) {
        lob::write_json_file(js, lob::to_json(to_bench_result(run)));
        std::printf("\nwrote %s\n", js.c_str());
    }
    const std::string csv = args.str("csv", "");
    if (!csv.empty()) {
        lob::write_hist_csv(csv, run.hist.bins());
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
            "usage: lob_bench --mode core|book|e2e [options]\n"
            "  core: full matching engine. --events N --warmup N --seed S --band-min P\n"
            "        --band-max P --mid P --pool N --clients N --p-new f --p-cancel f\n"
            "        --p-market f --p-aggressive f --depth-ticks N --pin --cpu N\n"
            "        --json file --csv file --no-book-events\n"
            "        --book bitset|map|map-pooled|flat   pick the order-book structure\n"
            "        --compare [--with-pooled-map]       run the books on identical flow\n"
            "  book: order-book operations only, no matching -- for cachegrind / perf.\n"
            "        same options; --book / --compare apply.\n"
            "  load: open-loop response time. --rate R (ops/s) or --rate-sweep a,b,c (M/s)\n"
            "        --uniform (default Poisson) --events N --depth-ticks N --pin --json file\n"
            "  e2e : --events N --port P --md-port P --seed S --json file\n");
        return 0;
    }
    if (mode == "e2e") return run_e2e(args);
    if (mode == "load") return run_load(args);
    return run_core(args, mode == "book");
}
