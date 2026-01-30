#!/usr/bin/env python3

import argparse
import os
import re
import statistics
from collections import defaultdict

def median(xs):
    return statistics.median(xs) if xs else None

def resolve_input_path(path):
    if os.path.isdir(path):
        cand = os.path.join(path, "benchmarklog.txt")
        if os.path.exists(cand):
            return cand
        return os.path.join(path, "testlog.txt")

    if os.path.exists(path):
        return path

    d = os.path.dirname(path)
    base = os.path.basename(path)
    if base == "benchmarklog.txt":
        return os.path.join(d, "testlog.txt")
    if base == "testlog.txt":
        return os.path.join(d, "benchmarklog.txt")
    return path

def load_benchmarks(path):
    pat = re.compile(r"^BENCHMARK: (.+): ([0-9.]+) (\w+)$")
    values = defaultdict(list)

    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pat.match(line.strip())
            if not m:
                continue
            name, v, unit = m.groups()
            if unit != "ns":
                continue
            values[name].append(float(v))

    return {k: median(v) for k, v in values.items()}

def split_category(full):
    if "_" not in full:
        return None, None
    return full.rsplit("_", 1)

def fmt(x):
    return f"{x:.2f}" if x is not None else "-"

def table_single(med):
    rows = defaultdict(dict)
    for full, v in med.items():
        cat, lib = split_category(full)
        if not cat:
            continue
        if cat == "poll_churn_mt":
            continue
        rows[cat][lib] = v

    order = [
        "poll_ping_pong",
        "workers",
        "async_ping_pong",
        "signal_delivery",
        "idle_invocations",
        "timer_overhead",
        "timer_many_active",
        "poll_churn",
    ]

    out = []
    out.append("| benchmark | evio | evio-uring | libev | libevent | libuv |")
    out.append("|---|---:|---:|---:|---:|---:|")
    for cat in order:
        if cat not in rows:
            continue
        r = rows[cat]
        out.append(
            f"| {cat} | {fmt(r.get('evio'))} | {fmt(r.get('evio-uring'))} | {fmt(r.get('libev'))} | {fmt(r.get('libevent'))} | {fmt(r.get('libuv'))} |"
        )
    return "\n".join(out)

def table_mt(med):
    mt = defaultdict(dict)  # threads -> lib -> value
    for full, v in med.items():
        cat, lib = split_category(full)
        if cat != "poll_churn_mt":
            continue
        if "-t" not in lib:
            continue
        base, t = lib.rsplit("-t", 1)
        try:
            threads = int(t)
        except ValueError:
            continue
        mt[threads][base] = v

    out = []
    out.append("| threads | evio | evio-uring | libev | libevent | libuv |")
    out.append("|---:|---:|---:|---:|---:|---:|")
    for threads in sorted(mt.keys()):
        r = mt[threads]
        out.append(
            f"| {threads} | {fmt(r.get('evio'))} | {fmt(r.get('evio-uring'))} | {fmt(r.get('libev'))} | {fmt(r.get('libevent'))} | {fmt(r.get('libuv'))} |"
        )
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="Path to meson-logs/ (or benchmarklog.txt/testlog.txt)")
    args = ap.parse_args()

    med = load_benchmarks(resolve_input_path(args.input))
    print(table_single(med))
    print()
    print(table_mt(med))

if __name__ == "__main__":
    main()
