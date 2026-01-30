#!/usr/bin/env python3

import os
import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np

def resolve_input_path(path):
    if os.path.isdir(path):
        cand = os.path.join(path, "benchmarklog.txt")
        if os.path.exists(cand):
            return cand
        cand = os.path.join(path, "testlog.txt")
        return cand

    if os.path.exists(path):
        return path

    d = os.path.dirname(path)
    base = os.path.basename(path)
    if base == "benchmarklog.txt":
        return os.path.join(d, "testlog.txt")
    if base == "testlog.txt":
        return os.path.join(d, "benchmarklog.txt")
    return path

def parse_log(file_path):
    # results[category][library] = value
    results = defaultdict(dict)
    versions = {}

    # BENCHMARK: <name>: <value> <unit>
    benchmark_pattern = re.compile(r"BENCHMARK: (.+): ([\d\.]+) (\w+)")
    version_patterns = {
        'evio': re.compile(r"evio:\s+([\d\.]+)"),
        'libev': re.compile(r"libev:\s+([\d\.]+)"),
        'libevent': re.compile(r"libevent:\s+(.+)"),
        'libuv': re.compile(r"libuv:\s+([\d\.\w-]+)")
    }

    if not os.path.exists(file_path):
        print(f"Error: Benchmark log file not found at '{file_path}'")
        print("Please run 'meson test --benchmark' first.")
        exit(1)

    with open(file_path, 'r') as f:
        for line in f:
            # Benchmark line
            match = benchmark_pattern.match(line)
            if match:
                full_name, value_str, unit = match.groups()
                value = float(value_str)

                # name is like 'category_lib'
                parts = full_name.rsplit('_', 1)
                if len(parts) == 2:
                    category, library = parts
                    results[category][library] = value
                continue

            # Version lines
            for lib, pattern in version_patterns.items():
                if lib not in versions:
                    match = pattern.search(line)
                    if match:
                        versions[lib] = match.group(1)

    return results, versions

def plot_results(results, versions, output_file):
    # One plot per category
    categories = sorted(list(results.keys()))
    num_categories = len(categories)

    if not num_categories:
        print("No benchmark results found to plot.")
        return

    version_parts = []
    if versions.get('evio'):
        version_parts.append(f"evio: {versions['evio']}")
    if versions.get('libev'):
        version_parts.append(f"libev: {versions['libev']}")
    if versions.get('libevent'):
        version_parts.append(f"libevent: {versions['libevent']}")
    if versions.get('libuv'):
        version_parts.append(f"libuv: {versions['libuv']}")
    version_str = ", ".join(version_parts)

    title = 'Benchmark Results (ns/op, lower is better)'
    if version_str:
        title += f'\n({version_str})'

    fig, axes = plt.subplots(num_categories, 1, figsize=(10, 5 * num_categories), squeeze=False)
    fig.suptitle(title, fontsize=16)

    for i, category in enumerate(categories):
        ax = axes[i][0]
        libs = sorted(list(results[category].keys()))
        values = [results[category][lib] for lib in libs]

        colors = plt.get_cmap('tab10').colors
        bars = ax.bar(libs, values, color=[colors[j % len(colors)] for j in range(len(libs))])
        ax.set_title(category.replace('_', ' ').title())
        ax.set_ylabel('Time (ns/op)')

        ax.bar_label(bars, fmt='%.2f')

    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    plt.savefig(output_file)
    print(f"Benchmark plot saved to {output_file}")

def main():
    parser = argparse.ArgumentParser(description="Plot benchmark results from Meson logs.")
    parser.add_argument('--input', required=True, help="Path to meson-logs/ (or benchmarklog.txt/testlog.txt)")
    parser.add_argument('--output', required=True, help="Path to output PNG file")
    args = parser.parse_args()

    results, versions = parse_log(resolve_input_path(args.input))
    plot_results(results, versions, args.output)

if __name__ == '__main__':
    main()
