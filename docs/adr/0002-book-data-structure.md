# 2. Order-book data structure: dense price array + hierarchical bitset

- Status: accepted
- Date: 2026-09-08

## Context

The book must support, on every order:

1. find the level for a known price (add / cancel / modify),
2. find the best bid and best ask (matching, top-of-book),
3. walk levels from the top inward (sweeps, market data, FOK sizing),
4. FIFO within a level (time priority).

Order arrival is bursty and the working set of prices is narrow and clustered
around the mid. Instruments trade inside exchange price bands, so the set of
*possible* prices is bounded and known up front.

## Options considered

| Option | best bid/ask | level lookup | notes |
|---|---|---|---|
| `std::map<Price, Level>` | O(log n) | O(log n) | simple, pointer-chasing, allocates per level |
| `boost::flat_map` | O(log n) | O(log n) | contiguous, but O(n) level insert/erase |
| Dense array of levels + 2-level bitset | O(1)* | O(1) | fixed memory over the band, cache-friendly |

\* two hardware bit-scans over a summary word.

## Decision

Each side is a `std::vector<PriceLevel>` indexed by `price - min_price`, so a
level lookup for a known price is one load. Occupancy is tracked in a
two-level bitset:

```
words[side]    : 1 bit per price level, set when the level is non-empty
summary[side]  : 1 bit per 64-level word, set when that word is non-zero
```

`best_bid` scans `summary` from the top for the highest set bit, then the
winning word, using `std::countl_zero`; `best_ask` scans from the bottom with
`std::countr_zero`. Orders are intrusive doubly-linked list nodes, so resting
and cancelling touch no allocator.

Memory is `band_width * (sizeof(PriceLevel) + ~2 bits) * 2 sides`. A 200k-tick
band is ~19 MB, allocated once.

## Consequences

- Best-price and level lookups are constant-time and cache-resident for a
  realistic book.
- Prices outside the configured band are rejected (`PRICE_OUT_OF_BAND`), which
  matches how real venues apply price bands.
- A pathologically wide band wastes memory. Mitigation: the band is a
  constructor parameter; callers size it to the instrument.
- Walking to the *next* occupied level (`next_level_toward_mid`) is currently a
  linear bit walk. For clustered books the gap is a handful of ticks; a
  word-at-a-time skip is a future optimisation if a use case needs it.
