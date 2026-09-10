# A reproducible comparison of order-book data structures

*Technical note accompanying the [low-latency LOB & matching engine](../README.md).*

## TL;DR

Running the **same** price-time-priority matching logic and the **same**
deterministic order stream through three different order-book data structures:

| structure | idea | level lookup | best price | insert / erase a level |
|---|---|---|---|---|
| **bitset** (this engine) | dense `PriceLevel[]` over the price band + 2-level occupancy bitset | O(1) | O(band / 64) bit-scan | O(1) |
| **`std::map`** | one `std::map<Price, PriceLevel>` per side | O(log L) | O(1) (`begin()`) | O(log L) |
| **sorted `std::vector`** | one price-sorted `vector` per side (a `flat_map`) | O(log L) | O(1) (`front()`) | **O(L)** memmove |

(L = number of occupied price levels.)

**Findings on an i3-1125G4 (Windows, GCC 16.2 `-O3`), 10 M commands per run:**

1. `std::map` is **within 0–4 %** of the bitset book on a *shallow* book
   (tens–hundreds of levels) and falls **~10–15 % behind** as the book widens to
   thousands of levels — the tree spills cache and every op eats `log L`
   pointer-chases. Under a **cancel-heavy** stream (50 % cancels) it is **1.5×
   slower** because of tree rebalancing on every level create/destroy.
2. The **sorted vector is a trap for a deep book**: 1.8× slower at ~hundreds of
   levels, **3–6× slower** and a p99.9 of **30–44 µs** (vs ~3 µs) at thousands
   of levels, because every level insert/erase memmoves the tail of the array.
   But on a *shallow, churny* book it is actually **~8 % faster** than the
   bitset — a small contiguous array with cheap memmoves beats a fixed-width
   bit-scan.
3. The **bitset book is the most predictable**: p99.9 stays **~2–4 µs across
   every configuration**, where `std::map` drifts up and the vector explodes.
   Its one structural cost is that `best_price` scans a bitset sized to the
   configured price *band*, not the live level count — so on a shallow book it
   carries dead weight it cannot shed.

The honest conclusion is **not** "the bitset book wins." It is: *the bitset book
is the safe default because its cost is flat across workloads; a sorted vector
is right only for shallow high-churn books; `std::map` is fine until the book
gets wide or the cancel rate gets high.*

## Why this is worth measuring

Every "fast limit order book" writeup picks a structure and reports one number
on one workload. The interesting question — *how does the choice interact with
book shape and order-flow mix?* — is rarely shown, and almost never
reproducibly. Here the matching engine is `template <class Book>`, the three
books present an identical surface, and a
[correctness test](../tests/test_book_equivalence.cpp) asserts that all three
produce **bit-identical** event streams and final books over 400 k-command runs
across four seeds. So the only variable between runs is the data structure.

## Method

- **Workload**: the deterministic `OrderFlowGenerator` (xoshiro256\*\*) — ~55 %
  new orders (28 % of limits cross the spread, 4 % market), ~35 % cancels,
  ~10 % modifies, mid price random-walking inside a 200 k-tick band. The
  `--depth-ticks` knob controls how far limit prices scatter from the mid,
  which controls **L**, the number of occupied price levels.
- **Measurement**: `lob_bench --mode core --compare`. Per-command latency via
  calibrated `rdtsc` with the probe cost subtracted; percentiles from an
  HdrHistogram; 1 M-command warm-up; throughput = wall time over the batch,
  flow generation excluded. See [ADR 7](adr/0007-latency-measurement.md).
- All three books run **back-to-back in one process** under the same thermal
  state, so the `vs bitset` ratios are apples-to-apples even though the
  absolute throughput drifts run-to-run on this passively-cooled chip.
- Pinned to one core, `HIGH_PRIORITY_CLASS`.

## Results

### Throughput and tail vs book width

`lob_bench --mode core --events 10000000 --seed 1 --pin --cpu 3 --depth-ticks <D> --compare`

| depth-ticks | book | throughput | ns/cmd | p50 | p99 | p99.9 | vs bitset |
|---:|---|---:|---:|---:|---:|---:|---:|
| **16** (shallow) | bitset | 3.95 M/s | 253 | 173 ns | 707 ns | 1.99 µs | 1.00× |
| | map | 3.94 M/s | 254 | 179 ns | 680 ns | 2.13 µs | 1.00× |
| | flat | 3.19 M/s | 313 | 176 ns | 3.80 µs | 8.28 µs | 1.24× |
| **48** | bitset | 3.67 M/s | 272 | 188 ns | 751 ns | 3.48 µs | 1.00× |
| | map | 3.54 M/s | 283 | 197 ns | 764 ns | 3.00 µs | 1.04× |
| | flat | 2.08 M/s | 482 | 228 ns | 6.34 µs | 14.7 µs | 1.77× |
| **200** | bitset | 3.27 M/s | 306 | 222 ns | 839 ns | 4.01 µs | 1.00× |
| | map | 2.94 M/s | 341 | 267 ns | 871 ns | 3.90 µs | 1.11× |
| | flat | 1.04 M/s | 965 | 307 ns | 14.0 µs | 29.2 µs | 3.16× |
| **800** (wide) | bitset | 3.44 M/s | 290 | 215 ns | 744 ns | 3.26 µs | 1.00× |
| | map | 2.98 M/s | 335 | 258 ns | 871 ns | 3.11 µs | 1.15× |
| | flat | 0.56 M/s | 1776 | 292 ns | 25.9 µs | 44.4 µs | 6.12× |

The `depth-ticks 200` row was re-run on seeds 2 and 3: `map` came out 1.14× and
1.05× — i.e. the ~1.1× gap is real, not noise.

![book comparison](images/book_comparison.png)

### Cancel-heavy stream

`--depth-ticks 120 --p-new 0.44 --p-cancel 0.5 --compare` (50 % of commands are
cancels → constant level creation and destruction):

| book | throughput | p50 | p99 | p99.9 | vs bitset |
|---|---:|---:|---:|---:|---:|
| bitset | 3.21 M/s | 222 ns | 934 ns | 3.66 µs | 1.00× |
| map | 2.13 M/s | 369 ns | 1.51 µs | 4.22 µs | **1.51×** |
| flat | 3.46 M/s | 198 ns | 758 ns | 2.59 µs | **0.93×** |

## Analysis

**Why `std::map` keeps up on a shallow book.** With a few hundred levels the
red-black tree is a few kilobytes and stays resident in L1/L2. `log L ≈ 8`
comparisons per op is cheap when every node is a cache hit. The bitset's O(1)
is a smaller constant, not a different order of magnitude, at this L.

**Why `std::map` slips as the book widens.** At thousands of levels the tree no
longer fits in L1; each of the `log L` steps down the tree is an increasingly
likely cache miss, and node allocation/free on level churn touches the
allocator. The bitset touches at most `band/64` contiguous machine words for a
best-price scan and a single word for an add/cancel — its cache footprint does
not grow with L.

**Why `std::map` collapses under churn.** Every cancel that empties a level is a
`map::erase` (rebalance + free); every new level is an `insert` (allocate +
rebalance). The bitset makes both a single bit flip.

**Why the sorted vector is fine when shallow and churny, fatal when wide.**
Insert/erase of a level is `memmove` of the array tail — O(L). When L is small
(≤ ~50) and the array is hot, that memmove is a handful of cache lines and
beats scanning a band-sized bitset. When L is thousands, every level change
shifts kilobytes, and because a cancel-heavy deep book changes levels
constantly, throughput falls off a cliff and the tail balloons to tens of µs.

**The bitset's structural cost.** `best_price` is `O(band / 64)` no matter how
few levels exist. Configure a 200 k-tick band and trade a stock that only uses
200 levels and you pay for 199 800 empty ticks on every top-of-book query.
Both baselines are `O(1)` here (`begin()` / `front()`). This is why the bitset
never *wins* by a wide margin — and why a real deployment sizes the band to the
instrument.

## Threats to validity

- **One CPU, thermally throttled, Windows.** Absolute throughput drifts
  1.8–3.9 M/s across the session; only the within-run `vs bitset` ratios are
  trustworthy. A tuned Linux host with `isolcpus` would tighten every number
  and lower the tails, but the *relative* ordering is a property of the
  algorithms, not the box.
- **No hardware performance counters.** The cache-miss explanation above is
  the textbook mechanism and is consistent with the L-dependence in the data,
  but it is not directly measured here — `perf stat` needs Linux. This is the
  first thing to add (`perf stat -e cache-misses,LLC-load-misses,...` per book
  in CI).
- **Synthetic order flow.** The generator is a reasonable zero-intelligence
  model but not a replay of a real venue. Book width and churn are the levers
  that matter for this comparison and both are swept, but a real ITCH replay
  would be a stronger workload.
- **`std::map` node allocator.** Default `std::allocator`; a pooled node
  allocator would narrow the churn gap. Out of scope here — the point is what
  you get from the standard container as written.

## Reproduce

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build -R BookEquivalence        # all three books agree

for D in 16 48 200 800; do
  ./build/lob_bench --mode core --events 10000000 --seed 1 --pin --cpu 3 \
      --depth-ticks $D --compare --json bench-out/compare_dt$D.json
  sleep 15   # let the CPU cool between runs
done
python tools/plot_results.py        # -> docs/images/book_comparison.png
```

Book implementations: [`order_book.hpp`](../include/lob/order_book.hpp) (bitset),
[`map_order_book.hpp`](../include/lob/map_order_book.hpp),
[`sorted_vector_order_book.hpp`](../include/lob/sorted_vector_order_book.hpp).
