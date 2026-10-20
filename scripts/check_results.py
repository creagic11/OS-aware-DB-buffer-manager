#!/usr/bin/env python3
"""Phase 8 validation: every experiment has raw CSV + plot, they agree, variance is shown.

Checks, per experiment in configs/experiments.toml:
  1. results/raw/<id>.csv and results/plots/<id>.png both exist (PNG is a valid image);
  2. the aggregates in results/plots/<id>_plotted.csv are recomputed from the raw CSV
     and must match (the plot reflects the CSV);
  3. every (config, x) point has >= 3 repetitions (no single unrepeated number);
  4. every row records its cache state and the p50/p99/IOPS columns are present.
Finally requires at least 5 complete experiments (the spec's floor).
"""

from __future__ import annotations

import sys
import tomllib
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from plot import aggregate  # noqa: E402


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--results", default=str(ROOT / "results"))
    ap.add_argument("--min-experiments", type=int, default=5)
    args = ap.parse_args()
    cfg = tomllib.loads((ROOT / "configs/experiments.toml").read_text())
    res = Path(args.results)
    ok_count, problems = 0, []
    for exp in cfg["experiment"]:
        i = exp["id"]
        raw, png, plotted = res / "raw" / f"{i}.csv", res / "plots" / f"{i}.png", res / "plots" / f"{i}_plotted.csv"
        missing = [p.name for p in (raw, png, plotted) if not p.exists()]
        if missing == [raw.name, png.name, plotted.name]:
            print(f"--  {i}: not run")
            continue
        if missing:
            problems.append(f"{i}: missing {missing}")
            continue
        if png.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
            problems.append(f"{i}: {png.name} is not a PNG")
            continue
        df = pd.read_csv(raw)
        for col in ("cache_state", "p50_us", "p99_us", "throughput_ops", "io_iops"):
            if col not in df or df[col].isna().any():
                problems.append(f"{i}: column {col} missing or empty")
        again = aggregate(df, exp["metrics"]).reset_index(drop=True)
        drawn = pd.read_csv(plotted)
        if len(again) != len(drawn):
            problems.append(f"{i}: plotted points {len(drawn)} != recomputed {len(again)}")
            continue
        for col in ("mean", "sd", "min", "max"):
            a = again[col].astype(float).to_numpy()
            b = drawn[col].astype(float).to_numpy()
            if not all(abs(x - y) <= 1e-9 * max(1.0, abs(x)) for x, y in zip(a, b)):
                problems.append(f"{i}: plotted {col} values do not match raw CSV")
        if (again["n"] < 3).any():
            problems.append(f"{i}: some points have < 3 repetitions")
        n_points = again.groupby(["series", "x_label"]).ngroups
        print(f"OK  {i}: {len(df)} runs, {n_points} configurations, n>={int(again['n'].min())}, plot consistent")
        ok_count += 1
    for p in problems:
        print("FAIL", p)
    print(f"{ok_count} complete experiments (floor: {args.min_experiments})")
    return 0 if ok_count >= args.min_experiments and not problems else 1


if __name__ == "__main__":
    raise SystemExit(main())
