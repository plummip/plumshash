#!/usr/bin/env python3
"""
bench_min7.py — published-table runner for bench_compete.

Runs ./bench_compete RUNS times (default 7) and takes the per-cell MINIMUM
across runs, then prints the two markdown tables that README.md carries.

Why min-of-runs: this SoC's walt governor drifts DVFS on a 10-100 ms scale, so
a single run can land a stray boost/drift bin inside one cell. Each run is
already internally min-of-3 over a shuffled (size, variant) order; taking the
min across runs approximates the true best-case cell. Absolute ns/op are still
not reproducible run-to-run — treat within-row ratios as the signal.

Usage:
    python3 bench_min7.py                 # 7 runs, unpinned (published basis)
    python3 bench_min7.py --runs 7 --pin 0   # pin to CPU 0 (needs taskset)
    python3 bench_min7.py --raw           # also echo each run's stdout

The tables go to stdout; `make bench` writes them to bench_results.txt
(gitignored) and then runs test_speed for raw bandwidth.
"""
import argparse
import os
import re
import subprocess
import sys

CELL_GB_NS = re.compile(r"(\d+\.\d+)\s*\(\s*(\d+\.\d+)\)")
CELL_NS = re.compile(r"(\d+\.\d+)ns")
ROW = re.compile(r"^\s*(\S+)\s*\|(.*)$")
HARNESS = re.compile(r"^=== (LATENCY|THROUGHPUT)")
HEADER_NAMES = ("Size", "------")


def parse(out):
    """-> {harness: {size_label: [cells]}} where cell = (gb_or_None, ns)."""
    data, harness = {}, None
    for line in out.splitlines():
        m = HARNESS.match(line.strip())
        if m:
            harness = m.group(1)
            data.setdefault(harness, {})
            continue
        if harness is None:
            continue
        r = ROW.match(line)
        if not r:
            continue
        label, rest = r.group(1), r.group(2)
        if label in HEADER_NAMES:
            continue
        gb_ns = CELL_GB_NS.findall(rest)
        if gb_ns:
            cells = [(float(g), float(n)) for g, n in gb_ns]
        else:
            cells = [(None, float(n)) for n in CELL_NS.findall(rest)]
        if cells:
            data[harness][label] = cells
    return data


def merge_min(best, cur):
    for harness, sizes in cur.items():
        dst = best.setdefault(harness, {})
        for label, cells in sizes.items():
            prev = dst.get(label)
            if prev is None or len(prev) != len(cells):
                dst[label] = list(cells)
                continue
            for i, (gb, ns) in enumerate(cells):
                pgb, pns = prev[i]
                best_gb = gb if pgb is None else (pgb if gb is None else min(pgb, gb))
                dst[label][i] = (best_gb, min(pns, ns))
    return best


def emit(best, runs, pin):
    names = ["plums", "rapidhash", "wyhash", "xxh3", "t1ha2"]
    basis = f"min-of-{runs} runs (each run min-of-3 over shuffled order)"
    if pin is not None:
        basis += f", pinned to CPU {pin}"
    else:
        basis += ", unpinned"
    for harness in ("LATENCY", "THROUGHPUT"):
        sizes = best.get(harness)
        if not sizes:
            print(f"!! no {harness} data captured", file=sys.stderr)
            continue
        print(f"=== {harness} ({basis}) ===")
        print("| Size | " + " | ".join(f"{n}" for n in names) + " |")
        print("|------|" + "|".join("-------" for _ in names) + "|")
        for label, cells in sizes.items():
            row = []
            for gb, ns in cells:
                row.append(f"{ns:.1f}ns" if gb is None else f"{ns:.1f}")
            print(f"| {label} | " + " | ".join(row) + " |")
        print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--pin", type=int, default=None,
                    help="pin the sweep to this CPU (e.g. 0 = a 3.6GHz perf core)")
    ap.add_argument("--binary", default="./bench_compete")
    ap.add_argument("--raw", action="store_true", help="echo each run's stdout")
    args = ap.parse_args()

    if args.pin is not None:
        # os.sched_setaffinity, not taskset: Android's toybox taskset has no -c,
        # and affinity is inherited by every child process anyway.
        try:
            os.sched_setaffinity(0, {args.pin})
            got = sorted(os.sched_getaffinity(0))
            print(f"# pinned to CPU {args.pin} (affinity {got})", file=sys.stderr)
        except (AttributeError, OSError) as e:
            print(f"!! cannot pin to CPU {args.pin}: {e}", file=sys.stderr)

    best = {}
    for run in range(1, args.runs + 1):
        print(f"# run {run}/{args.runs}", file=sys.stderr)
        out = subprocess.run([args.binary], capture_output=True, text=True, check=True).stdout
        if args.raw:
            print(out, end="")
        merge_min(best, parse(out))
    emit(best, args.runs, args.pin)


if __name__ == "__main__":
    main()
