#!/usr/bin/env python3
"""Render measured cache benchmark CSVs without third-party dependencies."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median


METRICS = [
    ("Torrent rebuilds", "rebuilds", "count"),
    ("Average rebuild duration", "average_rebuild_ms", "ms"),
    ("Maximum rebuild duration", "maximum_rebuild_ms", "max_ms"),
    ("Pieces reclaimed per rebuild", "evicted_per_rebuild", "pieces"),
    ("Pieces admitted per rebuild", "admitted_per_rebuild", "pieces"),
    ("Cache-window changes", "window_changes", "count"),
    ("Consumer stalls (wait >=50 ms)", "stalls", "count"),
    ("Total verified-data waiting", "wait_ms", "ms"),
    ("Consumer stall duration", "stall_ms", "ms"),
    ("Cache hit rate", "hit_percent", "percent"),
    ("Backend work, including hashing", "backend_ms", "ms"),
    ("Verified lease disk reads", "lease_read_ms", "ms"),
    ("Minimum free slots sampled", "min_free", "slots"),
    ("Maximum free slots sampled", "max_free", "slots"),
    ("Post-start VLC buffering states", "vlc_buffer_stalls", "count"),
    ("Post-start VLC buffering duration", "vlc_buffer_ms", "ms"),
]


def load_measurements(path):
    groups = defaultdict(list)
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            name = row.pop("workload").rsplit("-", 1)[0]
            measured = {key: float(value) for key, value in row.items()}
            for key in ("evicted", "admitted"):
                measured[key + "_per_rebuild"] = measured[key] / measured["rebuilds"] if measured["rebuilds"] else 0
            groups[name].append(measured)
    if not groups:
        raise ValueError(f"No measurements in {path}")
    return groups


def measured_value(rows, key, unit):
    values = [row[key] for row in rows]
    return max(values) if unit == "max_ms" else median(values)


def format_value(value, unit):
    if unit in ("count", "slots"):
        return f"{value:g}"
    if unit == "percent":
        return f"{value:.3f}%"
    if unit == "pieces":
        return f"{value:.3f}"
    return f"{value:.3f} ms"


def improvement(before, after, unit):
    if unit == "percent":
        return f"{after - before:+.3f} percentage points"
    if unit in ("slots", "pieces"):
        return f"{after - before:+.3f} {unit}"
    if before == after:
        return "unchanged"
    if before == 0:
        return "increased from zero"
    change = 100 * (before - after) / before
    return f"{abs(change):.1f}% {'lower' if change > 0 else 'higher'}"


def render_comparison(before, after):
    if set(before) != set(after):
        raise ValueError("Before/after workloads differ")
    lines = [
        "# Cache performance comparison", "",
        "Real loopback peers, verified disk cache and headless LibVLC. Values are",
        "medians of three runs; maximum rebuild duration is the largest observed",
        "rebuild across the three runs. Read stalls are a consumer-wait measure,",
        "separate from LibVLC buffering states. This is not a WAN benchmark.", "",
        "Raw data: [before](performance/before.csv), [after](performance/after.csv).",
        "See [the design and reproduction instructions](cache-optimization.md).", "",
    ]
    for name in before:
        if len(before[name]) != 3 or len(after[name]) != 3:
            raise ValueError(f"Expected three repetitions for {name}")
        title = "VLC" if name == "vlc" else name.capitalize()
        lines.extend([f"## {title}", "", "| Metric | Before | After | Change |",
                      "|---|---:|---:|---:|"])
        for label, key, unit in METRICS:
            original = measured_value(before[name], key, unit)
            optimized = measured_value(after[name], key, unit)
            lines.append(f"| {label} | {format_value(original, unit)} | {format_value(optimized, unit)} | "
                         f"{improvement(original, optimized, unit)} |")
        original_slots = before[name][0]
        optimized_slots = after[name][0]
        lines.extend(["", f"Managed disk allocation: {original_slots['allocated']:,.0f} -> "
                      f"{optimized_slots['allocated']:,.0f} bytes; budget {optimized_slots['budget']:,.0f} bytes.", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    report = render_comparison(load_measurements(arguments.before), load_measurements(arguments.after))
    arguments.output.write_text(report + "\n")


if __name__ == "__main__":
    main()
