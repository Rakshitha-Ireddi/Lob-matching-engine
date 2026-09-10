# 10. Thread-per-stage pipeline

- Status: accepted
- Date: 2026-09-09

## Context

`engine_server` has four jobs: read/parse order entry, match, publish market
data, and emit telemetry. They have very different duty cycles and only the
middle one is on the critical path.

## Decision

One thread per stage, wired by SPSC rings (ADR 6):

| Thread | Job | Polling |
|---|---|---|
| `gateway` | accept, `recv`, decode → inbound ring; drain exec ring → `send` | busy-poll (`epoll`/`WSAPoll` with 0 timeout) |
| `engine` | drain inbound ring → `process` → exec + md rings; periodic snapshot | busy-poll, `pause` when idle |
| `publisher` | drain md ring → encode → `sendto`; 2 Hz heartbeat | `pause` when idle, short sleep when quiet |
| `telemetry` | copy snapshot, add rates, write `stats.json` | sleeps `stats-interval-ms` |

`engine_cpu` in the config pins `engine` to a core and `gateway` to the next
one. Telemetry writes are write-to-temp + rename so a dashboard poll never
sees a partial file, and the snapshot the engine hands over is copied under a
mutex the hot loop never takes.

## Consequences

- The engine thread does nothing but match and (every 250 ms) snapshot. Parse
  cost, encode cost and file I/O are off the critical path.
- On a production host the three hot threads pin to isolated cores and never
  sleep. On a shared dev box that would starve everything else, so the
  publisher and (in `e2e`) the engine back off when idle -- this trades a
  little tail latency for not pinning the machine at 100%. The trade-off is a
  config/build concern, not a code change to the pipeline.
- Four threads is the floor; adding instruments means another engine thread and
  its own rings, not more work on the existing one.
