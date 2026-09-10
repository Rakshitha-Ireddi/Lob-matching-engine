# 4. One thread owns the book (single-writer matching core)

- Status: accepted
- Date: 2026-09-09

## Context

The matching core is the serialisation point of the whole system: price-time
priority is only well defined against a single, totally ordered stream of
commands. The tempting alternative -- shard the book by price range or
instrument and match in parallel -- adds cross-shard ordering problems and
locking exactly where the latency budget is smallest.

## Decision

`MatchingEngine` is single-threaded and holds no locks. Exactly one thread
ever calls `process()`. It owns the book, the order pool and the order index
outright. Other stages reach it only through single-producer/single-consumer
ring buffers (ADR 6):

```
gateway ──inbound ring──▶ engine ──exec ring──▶ gateway (TX)
                             └────md ring─────▶ publisher
```

`process(cmd)` returns a `std::span<const Event>` into an internal buffer that
is valid until the next call -- no per-event allocation, no callback
indirection on the hot path.

## Consequences

- The hot path has no atomics, no mutexes, no contention. Latency is a
  function of the work the command implies, not of scheduling.
- Throughput of the core is bounded by one core. Measured at ~2-3.3 M
  commands/s on a laptop i3; a server core clears that comfortably, and real
  venues run one book per core. Horizontal scale is one engine thread per
  instrument, not parallelism within a book.
- Snapshotting the book for the dashboard happens *on the engine thread*
  during its periodic tick and is copied out under a short mutex that the hot
  path never takes.
- Testing is trivial: feed commands, assert on the returned events. No
  concurrency in the unit tests for matching logic.
