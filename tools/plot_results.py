#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render benchmark charts from lob_bench output.

    python tools/plot_results.py --bench bench-out/core.json \
        --hist bench-out/core_hist.csv --snapshot web/stats.json \
        --outdir docs/images

Produces:
    latency_cdf.png        latency distribution (CDF, log-x)
    latency_hist.png       latency histogram (log-x)
    percentile_bars.png    p50/p90/p99/p99.9/p99.99 bar chart
    throughput.png         throughput across seeds
    book_depth.png         order-book depth ladder from the live snapshot
"""
import argparse
import csv
import glob
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

INK = "#0a0a0a"
GRID = "#d8d8d8"
BID = "#1f9d55"
ASK = "#d1495b"
ACCENT = "#2f6f9f"

plt.rcParams.update({
    "figure.facecolor": "white",
    "axes.facecolor": "white",
    "axes.edgecolor": "#888",
    "axes.labelcolor": INK,
    "text.color": INK,
    "xtick.color": INK,
    "ytick.color": INK,
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.6,
})


def load_hist(path):
    los, his, counts = [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            los.append(int(row["lo_ns"]))
            his.append(int(row["hi_ns"]))
            counts.append(int(row["count"]))
    return np.array(los), np.array(his), np.array(counts, dtype=float)


def fmt_ns(x, _pos=None):
    if x >= 1e9:
        return f"{x/1e9:g}s"
    if x >= 1e6:
        return f"{x/1e6:g}ms"
    if x >= 1e3:
        return f"{x/1e3:g}us"
    return f"{x:g}ns"


def plot_cdf(los, counts, bench, outdir):
    order = np.argsort(los)
    x = los[order]
    c = np.cumsum(counts[order])
    c = c / c[-1] * 100.0
    fig, ax = plt.subplots(figsize=(8, 4.2))
    ax.plot(x, c, color=ACCENT, linewidth=2)
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_xlabel("latency  (command → events emitted)")
    ax.set_ylabel("percentile")
    ax.set_ylim(0, 100)
    lat = bench["latency_ns"]
    for (key, yoff) in [("p50", 20), ("p99", -26), ("p999", 8)]:
        v = lat[key]
        ax.axvline(v, color="#bbb", linewidth=1, linestyle="--")
        ax.annotate(f"{key} = {fmt_ns(v)}", (v, 50), fontsize=9,
                    xytext=(7, yoff), textcoords="offset points")
    ax.set_title(f"Matching-engine latency CDF  ·  {bench['events']:,} events  ·  "
                 f"engine throughput {bench['throughput_ops']/1e6:.2f}M/s")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "latency_cdf.png"), dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_hist(los, his, counts, bench, outdir):
    mids = np.sqrt(np.maximum(los, 1) * np.maximum(his, 1))
    fig, ax = plt.subplots(figsize=(8, 4.2))
    ax.bar(mids, counts, width=mids * 0.18, color=ACCENT, edgecolor="none")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_xlabel("latency")
    ax.set_ylabel("samples")
    ax.set_title(f"Matching-engine latency histogram  —  median {fmt_ns(bench['latency_ns']['p50'])}")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "latency_hist.png"), dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_percentile_bars(seeds, outdir):
    keys = ["p50", "p90", "p99", "p999", "p9999"]
    labels = ["p50", "p90", "p99", "p99.9", "p99.99"]
    fig, ax = plt.subplots(figsize=(8, 4.2))
    width = 0.8 / len(seeds)
    xs = np.arange(len(keys))
    for i, (name, b) in enumerate(seeds):
        vals = [b["latency_ns"][k] for k in keys]
        ax.bar(xs + i * width, vals, width, label=name)
    ax.set_xticks(xs + width * (len(seeds) - 1) / 2)
    ax.set_xticklabels(labels)
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_ylabel("latency")
    ax.set_title("Latency percentiles across seeds (20M events each)")
    ax.legend(frameon=False, fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "percentile_bars.png"), dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_throughput(seeds, outdir):
    fig, ax = plt.subplots(figsize=(7, 4))
    names = [n for n, _ in seeds]
    vals = [b["throughput_ops"] / 1e6 for _, b in seeds]
    bars = ax.bar(names, vals, color=ACCENT, width=0.55)
    for bar, v in zip(bars, vals):
        ax.annotate(f"{v:.2f}M", (bar.get_x() + bar.get_width() / 2, v),
                    ha="center", va="bottom", fontsize=10)
    ax.set_ylabel("orders / sec  (millions)")
    ax.set_title("Engine steady-state throughput")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "throughput.png"), dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_book(snapshot, outdir):
    with open(snapshot) as f:
        s = json.load(f)
    rows = ([(b["price"], b["qty"], BID) for b in s["bids"]] +
            [(a["price"], a["qty"], ASK) for a in s["asks"]])
    rows.sort(key=lambda r: r[0])
    ys = np.arange(len(rows))
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.barh(ys, [r[1] for r in rows], color=[r[2] for r in rows], height=0.72)
    ax.set_yticks(ys)
    ax.set_yticklabels([f"{r[0]:,}" for r in rows])
    ax.set_xlabel("resting quantity")
    ax.set_ylabel("price (ticks)")
    ax.axhline(len(s["asks"]) - 0.5 if False else len([r for r in rows if r[2] == BID]) - 0.5,
               color="#444", linewidth=1)
    ax.set_title(f"{s['instrument']} book — bid {s['best_bid']:,} / ask {s['best_ask']:,}, "
                 f"spread {s['spread']}, {s['resting_orders']:,} resting")
    handles = [plt.Rectangle((0, 0), 1, 1, color=BID),
               plt.Rectangle((0, 0), 1, 1, color=ASK)]
    ax.legend(handles, ["bids", "asks"], frameon=False, loc="lower right")
    ax.grid(axis="y", visible=False)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "book_depth.png"), dpi=140, bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default="bench-out/core.json")
    ap.add_argument("--hist", default="bench-out/core_hist.csv")
    ap.add_argument("--snapshot", default="web/stats.json")
    ap.add_argument("--seeds-glob", default="bench-out/core_s*.json")
    ap.add_argument("--outdir", default="docs/images")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)

    bench = json.load(open(a.bench))
    los, his, counts = load_hist(a.hist)
    plot_cdf(los, counts, bench, a.outdir)
    plot_hist(los, his, counts, bench, a.outdir)

    seed_files = sorted(glob.glob(a.seeds_glob))
    seeds = [(os.path.basename(p).replace("core_", "").replace(".json", ""),
              json.load(open(p))) for p in seed_files]
    if seeds:
        plot_percentile_bars(seeds, a.outdir)
        plot_throughput(seeds, a.outdir)

    if os.path.exists(a.snapshot):
        plot_book(a.snapshot, a.outdir)

    print(f"wrote charts to {a.outdir}/")


if __name__ == "__main__":
    main()
