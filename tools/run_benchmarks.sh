#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build (if needed) and run the full benchmark set into bench-out/.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
BENCH="${BUILD}/lob_bench"
OUT=bench-out
EVENTS=${EVENTS:-50000000}
SEED_EVENTS=${SEED_EVENTS:-20000000}
PIN=${PIN:-"--pin --cpu 3"}

[ -x "$BENCH" ] || [ -x "$BENCH.exe" ] || {
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD"
}
mkdir -p "$OUT"

echo "== core (headline, ${EVENTS} events) =="
"$BENCH" --mode core --events "$EVENTS" --seed 1 $PIN \
  --json "$OUT/core.json" --csv "$OUT/core_hist.csv"

for s in 1 2 3; do
  echo "== core seed $s (${SEED_EVENTS} events) =="
  "$BENCH" --mode core --events "$SEED_EVENTS" --seed "$s" $PIN --json "$OUT/core_s$s.json"
done

echo "== e2e (loopback pipeline) =="
"$BENCH" --mode e2e --events 200000 --port 9040 --md-port 9041 --json "$OUT/e2e.json"

echo "== charts =="
python tools/plot_results.py

echo "done -> $OUT/ and docs/images/"
