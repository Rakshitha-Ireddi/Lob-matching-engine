# SPDX-License-Identifier: MIT
# End-to-end on Windows: build, test, benchmark, run the pipeline, refresh the
# dashboard snapshot and regenerate the charts.
#
#   pwsh tools/run_all.ps1
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

Write-Host ">> configure + build" -ForegroundColor Cyan
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

Write-Host ">> unit tests" -ForegroundColor Cyan
ctest --test-dir build --output-on-failure -j ([Environment]::ProcessorCount)

Write-Host ">> benchmarks" -ForegroundColor Cyan
New-Item -ItemType Directory -Force bench-out | Out-Null
& ./build/lob_bench.exe --mode core --events 50000000 --seed 1 --pin --cpu 3 `
    --json bench-out/core.json --csv bench-out/core_hist.csv
foreach ($s in 1..3) {
    & ./build/lob_bench.exe --mode core --events 20000000 --seed $s --pin --cpu 3 `
        --json "bench-out/core_s$s.json"
    Start-Sleep 20   # let the CPU cool between runs
}
& ./build/lob_bench.exe --mode e2e --events 200000 --json bench-out/e2e.json

Write-Host ">> live pipeline -> web/stats.json" -ForegroundColor Cyan
$eng = Start-Process -PassThru ./build/engine_server.exe -ArgumentList `
    "--port 9201 --md-port 9202 --pool 2000000 --stats-file `"$PWD/web/stats.json`" --run-seconds 45 --pin-cpu 2"
Start-Sleep 2
Start-Process ./build/md_simulator.exe -ArgumentList "--mode subscribe --md-port 9202 --duration 38"
Start-Sleep 1
& ./build/md_simulator.exe --mode flow --port 9201 --rate 45000 --duration 36 --seed 42 --pin --cpu 6 --json bench-out/flow.json
$eng.WaitForExit()

Write-Host ">> charts" -ForegroundColor Cyan
python tools/plot_results.py

Write-Host "`ndone. serve web/ for the dashboard; docs/benchmarks.md for the numbers." -ForegroundColor Green
