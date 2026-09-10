# 6. Stages communicate through bounded SPSC ring buffers

- Status: accepted
- Date: 2026-09-09

## Context

The gateway, engine and publisher run on separate threads (ADR 10) and need to
hand work across a thread boundary in one direction. A general MPMC queue or a
mutex + condvar would add contention and wake-up latency to the hot path.

## Decision

`SpscRing<T>` -- a bounded, wait-free, single-producer/single-consumer queue:

- Capacity rounded up to a power of two; index masking instead of modulo.
- `head` and `hint` on the producer's cache line, `tail` and `hint` on the
  consumer's, so the two threads never write the same line.
- Each side keeps a cached copy of the other side's index and only reloads it
  (an acquire) when its local view says the ring is full/empty -- most
  operations touch only local state.
- Full/empty is `(head + 1) == tail` / `head == tail`, so one slot is always
  left unused (no separate size counter to keep coherent).

The rings are sized generously (128k entries by default). If the engine
stalls long enough to fill the inbound ring, the gateway spins briefly then
drops and counts the command rather than blocking the socket thread.

## Consequences

- Cross-thread handoff is a couple of relaxed loads and one release store.
- Backpressure is explicit and observable (`inbound_drops`), not a hidden
  blocking dependency.
- Every message type crossing a ring must be trivially copyable; `Command`,
  `wire::ExecReport` and the internal `MdMsg` all are.
- SPSC only: adding a second producer or consumer to any ring is a design
  change, not a config change.
