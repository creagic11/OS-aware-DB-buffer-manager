#!/usr/bin/env python3
"""Run the BufferScope comparative experiment matrix (Phase 8).

Reads configs/experiments.toml, expands every experiment into bench_runner
invocations, and writes one raw CSV per experiment to <out>/raw/<id>.csv.

For each experiment it also records:
  * <out>/raw/iostat_<id>.log  - `iostat -x -y -t 1` for the whole experiment
  * <out>/raw/perf/<id>__*.txt - `perf stat` software counters per invocation
and once per session <out>/raw/environment.txt.

Memory-budget configurations run inside a memory cgroup (v1 or v2) capped at
calibrated-base-RSS + budget, so the OS page cache and the custom buffer pool get
the same amount of memory. Without cgroup permissions those configurations are
skipped with a warning (never silently run unconstrained).

Usage:
  scripts/run_experiments.py [--only e1_working_set,e3_hotspot] [--quick]
                             [--runs N] [--build build] [--data data/bench] [--out results]
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = 4096
HEADER = 32
KEY_LEN = 11  # "k%010u"
TINY_POOL = 16
FRAME_BYTES = 8192  # sizeof(Frame): canonical layout pads 4 KiB data + metadata to 8 KiB
SLACK_MIB = 4

# bench_runner flags that may appear in the config
FLAGS = {
    "pattern", "keys", "ops", "value_size", "read_pct", "delete_pct", "zipf_theta", "hot_keys",
    "hot_ops", "seed", "warmup_ops", "pool", "io_mode", "durability", "group_commit", "replacer",
    "cache", "runs",
}
BOOL_FLAGS = {"scramble"}
COMPUTED = {"pool_ratio", "pool_mib", "mem_budget_mib", "budget_use", "label", "name"}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def data_pages(keys: int, value_size: int) -> int:
    per_page = (PAGE - HEADER) // (4 + KEY_LEN + value_size + 4)
    return math.ceil(keys / per_page)


# ── cgroups ──────────────────────────────────────────────────────────────────
class Cgroup:
    def __init__(self, name: str):
        self.path = None
        self.v2 = False
        v1 = Path("/sys/fs/cgroup/memory")
        v2 = Path("/sys/fs/cgroup/cgroup.controllers")
        try:
            if v1.is_dir():
                self.path = v1 / name
            elif v2.exists() and "memory" in v2.read_text():
                self.path = Path("/sys/fs/cgroup") / name
                self.v2 = True
            if self.path is not None:
                self.path.mkdir(exist_ok=True)
                self._limit_file().write_text(str(1 << 40))
        except OSError as e:
            log(f"WARNING: memory cgroup unavailable ({e}); budget configs will be skipped")
            self.path = None

    @property
    def available(self) -> bool:
        return self.path is not None

    def _limit_file(self) -> Path:
        return self.path / ("memory.max" if self.v2 else "memory.limit_in_bytes")

    def set_limit_mib(self, mib: float) -> None:
        if not self.v2:
            # v1 refuses a limit below current usage; force-empty first.
            try:
                (self.path / "memory.force_empty").write_text("0")
            except OSError:
                pass
        self._limit_file().write_text(str(int(mib * (1 << 20))))

    def peak_mib(self) -> float:
        f = self.path / ("memory.peak" if self.v2 else "memory.max_usage_in_bytes")
        try:
            return int(f.read_text().strip()) / (1 << 20)
        except (OSError, ValueError):
            return float("nan")

    def reset_peak(self) -> None:
        if not self.v2:
            try:
                (self.path / "memory.max_usage_in_bytes").write_text("0")
            except OSError:
                pass

    def wrap(self, cmd: list[str]) -> list[str]:
        procs = self.path / "cgroup.procs"
        return ["sh", "-c", f'echo $$ > {procs} && exec "$@"', "sh", *cmd]


# ── tooling ──────────────────────────────────────────────────────────────────
def find_perf() -> str | None:
    cands = [shutil.which("perf")] + sorted(str(p) for p in Path("/usr/lib/linux-tools").glob("*/perf"))
    for c in cands:
        if not c:
            continue
        try:
            r = subprocess.run([c, "stat", "-e", "task-clock", "true"], capture_output=True, timeout=10)
            if r.returncode == 0:
                return c
        except (OSError, subprocess.TimeoutExpired):
            pass
    return None


def device_of(path: Path) -> str | None:
    try:
        st = os.stat(path)
        p = Path(f"/sys/dev/block/{os.major(st.st_dev)}:{os.minor(st.st_dev)}").resolve()
        return p.name
    except OSError:
        return None


def write_environment(out: Path, data_dir: Path) -> None:
    cmds = {
        "date": ["date", "-Iseconds"],
        "git": ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
        "uname": ["uname", "-a"],
        "lscpu": ["lscpu"],
        "free": ["free", "-m"],
        "findmnt": ["findmnt", "-T", str(data_dir)],
        "df": ["df", "-h", str(data_dir)],
        "nproc": ["nproc"],
    }
    with open(out / "environment.txt", "w") as f:
        for name, cmd in cmds.items():
            try:
                res = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout
            except OSError as e:
                res = str(e)
            f.write(f"## {name}\n{res}\n")
        dev = device_of(data_dir)
        if dev:
            for q in ("logical_block_size", "physical_block_size", "rotational", "scheduler"):
                for base in (Path(f"/sys/class/block/{dev}/queue"), Path(f"/sys/class/block/{dev}/../queue")):
                    if (base / q).exists():
                        f.write(f"## {dev} {q}\n{(base / q).read_text()}\n")
                        break


# ── expansion ────────────────────────────────────────────────────────────────
def expand(exp: dict, defaults: dict) -> list[dict]:
    runs = []
    sweep = exp.get("sweep", {"var": "_", "values": [None]})
    for value in sweep["values"]:
        for s in exp["series"]:
            p = dict(defaults)
            if "runs" in exp:
                p["runs"] = exp["runs"]
            p.update(exp.get("base", {}))
            if isinstance(value, dict):
                p.update({k: v for k, v in value.items() if k != "label"})
                x_label = value.get("label", str(value))
                x_value = x_label
            else:
                if value is not None:
                    p[sweep["var"]] = value
                x_label = str(value)
                x_value = value
            p.update({k: v for k, v in s.items() if k != "name"})
            if "pool" in s:
                p["_explicit_pool"] = True
            runs.append({"params": p, "series": s["name"], "x_label": x_label, "x_value": x_value})
    return runs


def resolve(p: dict) -> tuple[dict, float | None]:
    """Turn computed keys into concrete bench_runner params. Returns (params, budget)."""
    p = dict(p)
    pages = data_pages(int(p["keys"]), int(p.get("value_size", 100)))
    budget = p.pop("mem_budget_mib", None)
    use = p.pop("budget_use", None)
    if budget is not None:
        if use == "os":
            p["pool"] = TINY_POOL
        elif use == "split":
            p["pool"] = max(2, int(budget * (1 << 20) / 2 / FRAME_BYTES))
        elif use == "pool_capacity":
            # Equal DATA capacity: as many 4 KiB pages as the page cache would get. The
            # cgroup is sized for the real 8 KiB/frame footprint (see `limit` below), so
            # this isolates the replacement policy from the Frame-layout memory overhead.
            p["pool"] = max(2, int(budget * (1 << 20) / PAGE))
        else:  # "pool" (default): the whole budget, counted at real bytes per frame
            p["pool"] = max(2, int(budget * (1 << 20) / FRAME_BYTES))
        p.pop("pool_ratio", None)
        p.pop("pool_mib", None)
    else:
        if use == "os":
            p["pool"] = TINY_POOL
            p.pop("pool_ratio", None)
        if p.pop("_explicit_pool", False):  # a series' own `pool` beats a base pool_ratio
            p.pop("pool_ratio", None)
            p.pop("pool_mib", None)
        if "pool_ratio" in p:
            p["pool"] = max(2, round(float(p.pop("pool_ratio")) * pages))
        if "pool_mib" in p:
            p["pool"] = max(2, int(float(p.pop("pool_mib")) * 256))
    p["_data_pages"] = pages
    return p, budget


def command(runner: Path, p: dict, data_dir: Path, json_out: Path, label: str) -> list[str]:
    cmd = [str(runner), f"--dir={data_dir}", f"--json={json_out}", f"--label={label}", "--quiet"]
    for k, v in p.items():
        if k.startswith("_") or k in COMPUTED:  # internal / computed keys
            continue
        if k in BOOL_FLAGS:
            if v:
                cmd.append("--" + k.replace("_", "-"))
            continue
        if k not in FLAGS:
            raise SystemExit(f"unknown experiment parameter '{k}'")
        cmd.append(f"--{k.replace('_', '-')}={v}")
    return cmd


def calibrate_base_rss(runner: Path, p: dict, data_dir: Path, tmp: Path, cache: dict) -> tuple[float, float]:
    """(steady anonymous RSS, peak RSS) of a tiny-pool run: the memory every config pays."""
    key = (int(p["keys"]), int(p.get("value_size", 100)), int(p["ops"]), int(p.get("warmup_ops", 0)))
    if key in cache:
        return cache[key]
    q = dict(p)
    # Same op count as the job: per-op latency/op vectors are part of the steady footprint.
    q.update({"pool": TINY_POOL, "runs": 1, "cache": "warm", "durability": "off", "io_mode": "buffered"})
    out = tmp / "calib.json"
    subprocess.run(command(runner, q, data_dir, out, "calibration"), check=True)
    # Peak (VmHWM), not the end-of-run RSS: the key index's hash table rehashes during
    # the load phase and briefly needs more memory than it holds afterwards.
    r = json.loads(out.read_text())["runs"][0]
    base = (r["rss_anon_mib"], r["rss_peak_mib"])
    cache[key] = base
    log(f"  calibrated base RSS for keys={key[0]}: anon {base[0]:.1f} MiB, peak {base[1]:.1f} MiB")
    return base


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=str(ROOT / "configs/experiments.toml"))
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--data", default=str(ROOT / "data/bench"))
    ap.add_argument("--out", default=str(ROOT / "results"))
    ap.add_argument("--only", default="", help="comma-separated experiment ids")
    ap.add_argument("--runs", type=int, default=0, help="override repetitions")
    ap.add_argument("--quick", action="store_true", help="scale keys/ops down 10x (smoke run, not for reporting)")
    args = ap.parse_args()

    cfg = tomllib.loads(Path(args.config).read_text())
    runner = Path(args.build) / "bench_runner"
    if not runner.exists():
        raise SystemExit(f"{runner} not found - build first (cmake --build {args.build})")
    out = Path(args.out)
    raw = out / "raw"
    (raw / "perf").mkdir(parents=True, exist_ok=True)
    data_dir = Path(args.data)
    data_dir.mkdir(parents=True, exist_ok=True)
    tmp = out / "tmp"
    tmp.mkdir(exist_ok=True)
    write_environment(raw, data_dir)

    perf = find_perf()
    if not perf:
        log("WARNING: perf not usable; per-invocation perf stat logs will be absent")
    iostat = shutil.which("iostat")
    dev = device_of(data_dir)
    log(f"data dir {data_dir} on device {dev}; perf={perf or 'n/a'}; iostat={iostat or 'n/a'}")
    cg = Cgroup("bufferscope_bench")
    calib: dict = {}

    only = {x for x in args.only.split(",") if x}
    for exp in cfg["experiment"]:
        if only and exp["id"] not in only:
            continue
        log(f"=== {exp['id']}: {exp['title']}")
        jobs = expand(exp, cfg.get("defaults", {}))
        csv_path = raw / f"{exp['id']}.csv"
        rows: list[dict] = []
        io_proc = None
        if iostat:
            io_log = open(raw / f"iostat_{exp['id']}.log", "w")
            io_proc = subprocess.Popen([iostat, "-x", "-y", "-t", *( [dev] if dev else []), "1"], stdout=io_log,
                                       stderr=subprocess.DEVNULL)
        try:
            for j, job in enumerate(jobs):
                p = dict(job["params"])
                if args.quick:
                    p["keys"] = max(2000, int(p["keys"]) // 10)
                    p["ops"] = max(2000, int(p["ops"]) // 10)
                    if "warmup_ops" in p:
                        p["warmup_ops"] = max(1000, int(p["warmup_ops"]) // 10)
                if args.runs:
                    p["runs"] = args.runs
                p, budget = resolve(p)
                label = f"{exp['id']}|{job['series']}|{job['x_label']}"
                json_out = tmp / f"{exp['id']}_{j}.json"
                cmd = command(runner, p, data_dir, json_out, label)
                limit = None
                if budget is not None:
                    if not cg.available:
                        log(f"  SKIP (no cgroup): {label}")
                        continue
                    anon, peak = calibrate_base_rss(runner, p, data_dir, tmp, calib)
                    use = job["params"].get("budget_use", "pool")
                    if use == "os":
                        # Page cache is reclaimable, so no slack: everything above the process's
                        # steady anonymous memory is the page-cache budget.
                        limit = max(anon + budget, peak + 1)
                    else:
                        footprint = budget * FRAME_BYTES / PAGE if use == "pool_capacity" else budget
                        limit = peak + footprint + SLACK_MIB  # slack only protects anon memory
                    json_out.unlink(missing_ok=True)
                    cg.set_limit_mib(limit)
                    cg.reset_peak()
                    cmd = cg.wrap(cmd)
                if perf:
                    safe = "".join(c if c.isalnum() else "_" for c in label)[:150]
                    cmd = [perf, "stat", "-x", ",", "-o", str(raw / "perf" / f"{safe}.txt"), "-e",
                           "task-clock,context-switches,cpu-migrations,page-faults,major-faults,minor-faults",
                           "--", *cmd]
                log(f"  [{j + 1}/{len(jobs)}] {job['series']} @ {job['x_label']}  pool={p.get('pool')} "
                    f"io={p.get('io_mode')} runs={p.get('runs')}" + (f" cgroup={limit:.0f}MiB" if limit else ""))
                t0 = time.time()
                res = subprocess.run(cmd, capture_output=True, text=True)
                # perf stat can mask a child killed by a signal; trust the output file.
                if res.returncode != 0 or not json_out.exists():
                    log(f"  FAILED rc={res.returncode} (missing output; OOM in cgroup?): "
                        f"{res.stderr.strip()[-400:]}")
                    continue
                data = json.loads(json_out.read_text())
                for r in data["runs"]:
                    hits, misses = r.get("hits", 0), r.get("misses", 0)
                    acc = (hits + misses) * PAGE
                    shr = 1 - r.get("os_read_bytes", 0) / acc if acc else float("nan")
                    r.update({
                        "experiment": exp["id"], "series": job["series"], "x_label": job["x_label"],
                        "x_value": job["x_value"], "mem_budget_mib": budget if budget is not None else "",
                        "cgroup_limit_mib": round(limit, 1) if limit else "",
                        "cgroup_peak_mib": round(cg.peak_mib(), 1) if limit else "",
                        "storage_hit_ratio": max(0.0, min(1.0, shr)) if acc else "",
                        "data_pages_est": p["_data_pages"],
                    })
                    rows.append(r)
                log(f"     done in {time.time() - t0:.1f}s: "
                    + ", ".join(f"{m}={sum(float(r[m]) for r in data['runs']) / len(data['runs']):.4g}"
                                for m in ("throughput_ops", "p99_us", "hit_ratio")))
        finally:
            if io_proc:
                io_proc.terminate()
                io_proc.wait()
        if rows:
            first = ["experiment", "series", "x_label", "x_value"]
            cols = first + [c for c in rows[0].keys() if c not in first]
            for r in rows:
                for c in r:
                    if c not in cols:
                        cols.append(c)
            with open(csv_path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=cols)
                w.writeheader()
                w.writerows(rows)
            log(f"  wrote {csv_path} ({len(rows)} rows)")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
