# 7. Latency measurement methodology

- Status: accepted
- Date: 2026-09-10

## Context

Sub-microsecond measurements are easy to get wrong: the timer call costs as
much as the thing being timed, histograms lose the tail, and a busy OS adds
milliseconds of scheduler noise that has nothing to do with the code.

## Decisions

**Clock.** The per-operation delta in `lob_bench --mode core` is taken with
`rdtsc` (`lfence; rdtsc`, ~6-13 ns), not `std::chrono` (~20-30 ns via
`QueryPerformanceCounter` / `clock_gettime`). The TSC is calibrated once
against the monotonic clock at start-up. The probe cost is measured
(`rdtsc; rdtsc` back to back, minimum of 200k trials) and subtracted from
every sample.

**What is timed.** `core` mode times from immediately before
`MatchingEngine::process(cmd)` to immediately after -- i.e. command in, all
resulting events emitted. Flow generation happens outside the timed region.
Throughput is wall time over the batch, excluding generation.

**Histogram.** A from-scratch HdrHistogram (`LatencyHistogram`): log-linear
sub-buckets, 3 significant figures, constant relative error from 1 ns to 60 s,
O(1) record, no allocation after construction. Percentiles are reported as the
upper bound of the containing bucket, so latency is never under-reported.
Validated in `tests/test_latency_histogram.cpp` against a known uniform
distribution (<2% error at p50/p90/p99).

**Warm-up.** The first 1 M events (configurable) are processed but not
recorded.

**Coordinated omission.** `core` mode is a closed-loop micro-benchmark with no
external arrival schedule, so classic coordinated omission does not apply --
but for the same reason it does not model queueing delay under a fixed offered
load. `e2e` mode does (the client sends on a wall-clock schedule) and reports
the wire round-trip separately.

## Consequences

- Reported median/p90/p99 reflect the engine. p99.9 and beyond on an untuned
  multi-tasking OS include scheduler preemption of the benchmark thread; the
  single worst sample per run is routinely a 1-70 ms outlier and is called out
  as such rather than hidden. `docs/benchmarks.md` explains the gap to a tuned
  Linux target.
- `--pin` sets thread affinity and raises priority; on Linux it also attempts
  `SCHED_FIFO`. What actually took effect is printed.
