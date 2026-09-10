# LOB Console (dashboard)

Single-file operations dashboard for the matching engine. No build step: React,
Tailwind and htm are vendored under `vendor/`.

## Run

`engine_server` writes a snapshot a few times a second:

```bash
./build/engine_server --stats-file "$PWD/web/stats.json" --run-seconds 60
```

Serve this folder with any static server and open it:

```bash
python -m http.server 8777 --directory web
# http://localhost:8777/
```

`?view=overview|book|latency|feed` opens a specific tab directly (used when
capturing the README screenshots). The layout collapses the sidebar and
stacks the cards on narrow viewports.

## Views

| View | Shows |
|---|---|
| Overview | throughput, engine p50/p99, trades, resting orders, top of book, latency distribution, recent prints |
| Order Book | live depth ladder (bid/ask, cumulative size bars), trade tape |
| Latency | HdrHistogram of engine `command → events` latency, percentile table |
| Market Data | UDP feed message rate, bandwidth, sequence-gap count |

Data contract is the JSON emitted by `lob::to_json(const DashboardSnapshot&)`
in `src/telemetry.cpp`.
