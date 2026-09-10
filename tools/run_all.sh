#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# End-to-end: build, test, benchmark, run the pipeline, refresh the dashboard
# snapshot and regenerate every chart in docs/images/.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
BIN=$BUILD
[ -x "$BIN/lob_bench.exe" ] && EXT=.exe || EXT=""

echo ">> configure + build"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

echo ">> unit tests"
ctest --test-dir "$BUILD" --output-on-failure -j"$(nproc)"

echo ">> benchmarks"
tools/run_benchmarks.sh

echo ">> live pipeline (25s) -> web/stats.json"
"$BIN/engine_server$EXT" --port 9201 --md-port 9202 --pool 2000000 \
  --stats-file "$PWD/web/stats.json" --run-seconds 40 --pin-cpu 2 &
ENG=$!
sleep 2
"$BIN/md_simulator$EXT" --mode subscribe --md-port 9202 --duration 32 &
sleep 1
"$BIN/md_simulator$EXT" --mode flow --port 9201 --rate 45000 --duration 30 --seed 42 \
  --pin --cpu 6 --json bench-out/flow.json
wait $ENG || true

echo ">> charts"
python tools/plot_results.py

echo
echo "done. open web/index.html (any static server) for the dashboard,"
echo "     docs/benchmarks.md for the numbers."
