# 8. Binary wire protocol: fixed-size, little-endian, explicit codec

- Status: accepted
- Date: 2026-09-09

## Context

Order entry (TCP) and market data (UDP) need a format that decodes in a few
nanoseconds, frames unambiguously on a byte stream, and replays identically
from a capture.

## Decision

- **8-byte header**: `type (u8)`, `version (u8)`, `length (u16, whole frame)`,
  `seq (u32, per-stream)`. Then a fixed-size body per message type.
- **Little-endian**, encoded and decoded field by field through a small
  `Writer` / `Reader` (`lob/wire.hpp`). No `reinterpret_cast` of the buffer to
  a struct: that keeps it correct under strict aliasing and on a big-endian
  host, and the bounds checks fall out naturally.
- **No text, no varints, no optional fields.** Every message is a known size;
  `peek_header` returns `nullopt` until `length` bytes are present, so TCP
  reassembly is a length check.
- Messages: `NewOrder` / `CancelOrder` / `ModifyOrder` inbound; `ExecReport`
  outbound; `MdTrade` / `MdBookChange` / `MdHeartbeat` on multicast. Client
  send timestamps ride in `NewOrder` and are echoed in `ExecReport` for
  round-trip measurement.

## Consequences

- Decode is branch-light and allocation-free; round-trip is covered by
  `tests/test_wire.cpp` including truncation and wrong-type cases.
- Adding a field is a breaking change (bump `version`, new length constant).
  Acceptable for a system with one first-party client library.
- On x86-64 the explicit LE codec compiles to essentially the same loads as a
  packed-struct cast would, with none of the UB.
