# Architecture

## Data flow

```
                        TCP :9001 (order entry)                 UDP :9999 (market data, multicast)
                               │                                        ▲
                               ▼                                        │
   ┌───────────────────────────────────────────────────────────────────────────────┐
   │  engine_server                                                                │
   │                                                                               │
   │   ┌──────────┐  Command   ┌──────────────┐  ExecReport  ┌──────────┐           │
   │   │ gateway  │──────────▶ │              │ ───────────▶ │ gateway  │──▶ client │
   │   │  thread  │  (SPSC)    │   matching   │   (SPSC)     │   (TX)   │           │
   │   │ epoll rx │            │    engine    │              └──────────┘           │
   │   └──────────┘            │   (1 thread, │                                     │
   │                          │   owns book) │  MdMsg    ┌───────────┐              │
   │                          │              │ ────────▶ │ publisher │──▶ multicast │
   │                          └──────┬───────┘  (SPSC)   │  thread   │              │
   │                                 │ snapshot (250ms)  └───────────┘              │
   │                                 ▼                                             │
   │                          ┌────────────┐   stats.json    ┌───────────────┐      │
   │                          │ telemetry  │ ──────────────▶ │  web/index.html│     │
   │                          │  thread    │                 │  (dashboard)   │     │
   │                          └────────────┘                 └───────────────┘      │
   └───────────────────────────────────────────────────────────────────────────────┘
```

Every arrow between threads is a bounded single-producer/single-consumer ring
buffer (ADR 6). The matching engine is the only writer of the book (ADR 4).

## Components

### Core library (`include/lob`, `src`)

| Unit | Responsibility |
|---|---|
| `types.hpp` | domain types: `Price` (int ticks), `Quantity`, `Side`, `OrderType`, reject reasons |
| `order.hpp` / `object_pool.hpp` | intrusive order node; fixed-capacity free-list pool |
| `price_level.hpp` | one price point: FIFO queue + cached aggregate qty / count |
| `order_book.hpp` | dense price array + two-level occupancy bitset; best-price bit-scans; depth snapshot (ADR 2) |
| `flat_hash_map.hpp` | open-addressing `OrderId → Order*` index, backward-shift delete |
| `matching_engine.hpp` / `.cpp` | price-time-priority matching; Limit / Market / IOC / FOK / PostOnly; STP; cancel; modify (ADR 5); emits `Event`s |
| `spsc_ring.hpp` | wait-free bounded SPSC queue, cache-line-isolated indices (ADR 6) |
| `wire.hpp` / `.cpp` | binary little-endian protocol, explicit field codec (ADR 8) |
| `net.hpp` / `.cpp` | RAII sockets, `epoll`/`WSAPoll` poller, UDP multicast (ADR 9) |
| `latency_histogram.hpp` | HdrHistogram (log-linear buckets, 3 s.f.), percentiles, log-spaced display bins (ADR 7) |
| `order_flow.hpp` / `.cpp` | deterministic synthetic order-flow generator (xoshiro256\*\*) |
| `engine_server.hpp` / `.cpp` | the four-thread pipeline (ADR 10) |
| `telemetry.hpp` / `.cpp` | `DashboardSnapshot` + `BenchResult` → JSON, atomic file write |
| `affinity.hpp` / `.cpp` | thread pinning + priority, Windows and Linux |

### Executables (`apps`)

- **`engine_server`** — runs the pipeline; TCP order entry, UDP market data,
  optional `stats.json` for the dashboard.
- **`md_simulator`** — `--mode flow` drives a synthetic order stream over TCP
  and measures order-entry round-trip; `--mode subscribe` joins the multicast
  feed and reports rate / bandwidth / sequence gaps.
- **`lob_bench`** — `--mode core` micro-benchmarks `MatchingEngine::process`;
  `--mode e2e` measures the client wire round-trip through the full pipeline.

### Frontend (`web`)

Single-file dashboard (`web/index.html`, React + Tailwind via CDN, no build
step). Polls `stats.json` once a second and renders four views: Overview,
Order Book (live depth ladder + trade tape), Latency (HdrHistogram +
percentiles), Market Data (feed rate, bandwidth, sequence gaps).

## Matching algorithm

For an incoming order:

1. validate (quantity > 0, price in band, id not live); for `PostOnly` reject
   if marketable; for `FOK` reject unless the full quantity is available.
2. emit `Accepted` (taker ack).
3. while quantity remains and the opposite top-of-book crosses the limit:
   walk that price level head-first (oldest order = time priority), producing
   a taker `Fill`, a maker `Fill` and a public `Trade` per match; drop fully
   filled resting orders back to the pool.
4. remainder: rest it (`Limit`/`PostOnly`) or cancel it (`Market`/`IOC`/`FOK`).
5. if the top of book moved, emit `BookChanged`.

All events for one command are returned as a `std::span` into a reused buffer.

## Threading & memory

- No allocation on the hot path: orders come from `ObjectPool`, events from a
  reused `std::vector`, the book and index are sized once.
- No locks on the hot path: SPSC rings between stages; the only mutex guards
  the 250 ms dashboard snapshot copy, which the engine takes itself.
- Determinism: same seed + config ⇒ identical command stream ⇒ identical trade
  sequence and final book (`tests/test_determinism.cpp`).
