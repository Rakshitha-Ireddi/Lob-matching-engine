# 3. Prices are integer ticks, not floating point

- Status: accepted
- Date: 2026-09-08

## Context

Prices are compared for equality and ordering on every match, used to index
the level array, and carried on the wire and into market data. `double`
cannot represent most decimal tick values exactly (`0.1` is not
representable), so `a == b` and `a + tick` are unsafe, and the same price can
hash/compare two ways.

## Decision

`Price` is `std::int64_t`, counting ticks from zero. The tick size is a
property of the instrument and lives outside the engine (the wire and the UI
convert ticks to a display price). `kNoPrice = INT64_MIN` marks "no price"
(empty level, market order).

Quantities are `std::uint64_t` lots. No fractional quantities.

## Consequences

- Price comparison, arithmetic and array indexing are exact and branch-free.
- `int64` ticks cover any realistic price × tick-precision with enormous
  headroom (a $1,000,000 instrument at $0.0001 ticks is 10^10, ~30 bits).
- Callers that think in decimal prices must multiply by the tick reciprocal at
  the boundary. This is a one-line conversion and keeps the core integer-only.
- Market orders are modelled as a limit at `kNoPrice`; `OrderBook::crosses`
  treats `kNoPrice` as "always crosses".
