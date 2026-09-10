# 5. Modify keeps queue priority only on a same-price size-down

- Status: accepted
- Date: 2026-09-09

## Context

An amend can change price, quantity, or both. Whether the order keeps its
place in the FIFO queue matters: it is the difference between "trim my size"
and "jump the queue".

## Decision

Follow the common exchange convention:

| Change | Queue priority | Mechanism |
|---|---|---|
| Same price, quantity **down** | **kept** | reduce in place, emit `Replaced` |
| Same price, quantity **up** | lost | cancel + re-add at the tail |
| Price change (any quantity) | lost | cancel + re-add at the new level |
| Quantity to 0 | n/a | treated as a cancel |

`handle_modify` on a priority-losing change removes the order, releases it to
the pool, and replays the amend as a fresh `New` command with the original
order id -- so a repriced order that now crosses will match immediately, just
like a new aggressive order would.

## Consequences

- An order id can leave and re-enter the book within one `process()` call. The
  id is stable; the queue position is not.
- Size-down is O(1) and emits a single `Replaced` event. Reprice/size-up costs
  a cancel + a full `New` (which may match), and emits the corresponding
  events.
- A modify is not counted as a `new_order` in the stats even though it reuses
  `handle_new` internally.
