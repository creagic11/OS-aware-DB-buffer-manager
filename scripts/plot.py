#!/usr/bin/env python3
"""Generate one plot (+ aggregate table) per experiment from the raw CSVs.

For every results/raw/<id>.csv this writes
  results/plots/<id>.png          - one panel per metric, mean ± 1 sd error bars,
                                    thin whiskers = min..max across repetitions
  results/plots/<id>_plotted.csv  - the exact aggregates drawn (for check_results.py)
  results/summary/<id>.md         - markdown table: mean ± sd [min-max], n runs

Only measured rows are used; nothing is interpolated or estimated.
"""

from __future__ import annotations

import argparse
import math
import tomllib
from pathlib import Path

import warnings

import matplotlib

warnings.filterwarnings("ignore", message="Unable to import Axes3D")

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

NICE = {
    "hit_ratio": "buffer-pool hit ratio",
    "storage_hit_ratio": "storage hit ratio (no device read)",
    "p50_us": "p50 latency (µs)",
    "p99_us": "p99 latency (µs)",
    "throughput_ops": "throughput (ops/s)",
    "io_iops": "page I/Os issued per s",
    "dev_reads": "device reads",
    "dev_writes": "device writes",
    "dev_read_kib": "device read (KiB)",
    "dev_util_pct": "device util (%)",
    "cpu_util": "CPU (cores)",
    "cpu_sys_s": "system CPU (s)",
    "evictions": "evictions",
    "page_flushes": "page write-backs",
    "wal_syncs": "WAL fdatasyncs",
    "wal_sync_ms": "time in WAL fdatasync (ms)",
    "minor_faults": "minor page faults",
}
LOG_Y = {"throughput_ops", "p99_us", "p50_us", "dev_reads", "dev_read_kib", "io_iops", "evictions",
         "page_flushes", "wal_syncs", "wal_sync_ms", "dev_writes", "minor_faults"}


def aggregate(df: pd.DataFrame, metrics: list[str]) -> pd.DataFrame:
    order_x = list(dict.fromkeys(df["x_label"]))
    order_s = list(dict.fromkeys(df["series"]))
    rows = []
    for s in order_s:
        for x in order_x:
            g = df[(df["series"] == s) & (df["x_label"] == x)]
            if g.empty:
                continue
            for m in metrics:
                if m not in g:
                    continue
                v = pd.to_numeric(g[m], errors="coerce").dropna()
                if v.empty:
                    continue
                rows.append({"series": s, "x_label": x, "x_value": g["x_value"].iloc[0], "metric": m,
                             "mean": v.mean(), "sd": v.std(ddof=1) if len(v) > 1 else 0.0,
                             "min": v.min(), "max": v.max(), "n": len(v),
                             "cache_state": ",".join(sorted(set(g["cache_state"].astype(str))))})
    return pd.DataFrame(rows)


def numeric_x(agg: pd.DataFrame) -> bool:
    try:
        [float(v) for v in agg["x_value"]]
        return True
    except (TypeError, ValueError):
        return False


def plot(exp: dict, agg: pd.DataFrame, out_png: Path) -> None:
    metrics = [m for m in exp["metrics"] if m in set(agg["metric"])]
    n = len(metrics)
    cols = 3 if n > 4 else min(n, 2) or 1
    rows = math.ceil(n / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(5.2 * cols, 3.8 * rows), squeeze=False)
    is_num = numeric_x(agg)
    x_order = list(dict.fromkeys(agg["x_label"]))
    series = list(dict.fromkeys(agg["series"]))
    width = 0.8 / max(1, len(series))
    for i, m in enumerate(metrics):
        ax = axes[i // cols][i % cols]
        for k, s in enumerate(series):
            d = agg[(agg["metric"] == m) & (agg["series"] == s)]
            if d.empty:
                continue
            if is_num:
                xs = d["x_value"].astype(float).to_numpy()
            else:
                xs = [x_order.index(x) + (k - (len(series) - 1) / 2) * width for x in d["x_label"]]
            ax.errorbar(xs, d["mean"], yerr=d["sd"], marker="o", ms=4, capsize=3, lw=1.6 if is_num else 0,
                        elinewidth=1.2, label=s)
            ax.vlines(xs, d["min"], d["max"], colors="grey", lw=0.6, alpha=0.6)
        if is_num:
            vals = sorted({float(v) for v in agg["x_value"]})
            if vals and vals[0] > 0 and vals[-1] / vals[0] >= 8:
                ax.set_xscale("log", base=2)
                ax.set_xticks(vals)
                ax.set_xticklabels([f"{v:g}" for v in vals])
        else:
            ax.set_xticks(range(len(x_order)))
            ax.set_xticklabels(x_order, rotation=12, fontsize=8)
        if m in LOG_Y and (agg[agg["metric"] == m]["min"] > 0).all():
            ax.set_yscale("log")
        ax.set_title(NICE.get(m, m), fontsize=10)
        ax.set_xlabel(exp.get("x_label", ""), fontsize=8)
        ax.grid(alpha=0.3)
    for j in range(n, rows * cols):
        axes[j // cols][j % cols].axis("off")
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.965), ncol=min(4, len(labels)), fontsize=9,
               frameon=False)
    caches = ",".join(sorted(set(",".join(agg["cache_state"]).split(","))))
    nmin = int(agg["n"].min())
    fig.suptitle(f"{exp['title']}   (cache: {caches}; error bars ±1 sd, whiskers min–max, n≥{nmin})",
                 fontsize=11, y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out_png, dpi=130)
    plt.close(fig)


def fmt(v: float) -> str:
    if v == 0:
        return "0"
    a = abs(v)
    if a >= 1000:
        return f"{v:,.0f}"
    if a >= 10:
        return f"{v:.1f}"
    if a >= 1:
        return f"{v:.2f}"
    return f"{v:.3f}"


def summary_md(exp: dict, agg: pd.DataFrame, path: Path) -> None:
    metrics = [m for m in exp["metrics"] if m in set(agg["metric"])]
    lines = [f"### {exp['title']}", "", f"*{exp['question']}*", "",
             "| " + exp.get("x_label", "x") + " | config | cache | n | " + " | ".join(NICE.get(m, m) for m in metrics) + " |",
             "|" + "---|" * (4 + len(metrics))]
    for x in dict.fromkeys(agg["x_label"]):
        for s in dict.fromkeys(agg["series"]):
            d = agg[(agg["x_label"] == x) & (agg["series"] == s)]
            if d.empty:
                continue
            cells = []
            for m in metrics:
                r = d[d["metric"] == m]
                if r.empty:
                    cells.append("–")
                    continue
                r = r.iloc[0]
                cells.append(f"{fmt(r['mean'])} ± {fmt(r['sd'])} [{fmt(r['min'])}–{fmt(r['max'])}]")
            lines.append(f"| {x} | {s} | {d['cache_state'].iloc[0]} | {int(d['n'].iloc[0])} | " + " | ".join(cells) + " |")
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default=str(ROOT / "configs/experiments.toml"))
    ap.add_argument("--results", default=str(ROOT / "results"))
    args = ap.parse_args()
    cfg = tomllib.loads(Path(args.config).read_text())
    res = Path(args.results)
    (res / "plots").mkdir(parents=True, exist_ok=True)
    (res / "summary").mkdir(parents=True, exist_ok=True)
    made = 0
    for exp in cfg["experiment"]:
        raw = res / "raw" / f"{exp['id']}.csv"
        if not raw.exists():
            print(f"skip {exp['id']}: no raw data")
            continue
        df = pd.read_csv(raw)
        agg = aggregate(df, exp["metrics"])
        agg.to_csv(res / "plots" / f"{exp['id']}_plotted.csv", index=False)
        plot(exp, agg, res / "plots" / f"{exp['id']}.png")
        summary_md(exp, agg, res / "summary" / f"{exp['id']}.md")
        made += 1
        print(f"plotted {exp['id']} ({len(df)} raw rows)")
    print(f"{made} experiment plots written to {res / 'plots'}")
    return 0 if made else 1


if __name__ == "__main__":
    raise SystemExit(main())
