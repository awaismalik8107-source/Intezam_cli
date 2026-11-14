#!/usr/bin/env python3
"""Summarize the reproducible engine 2.0 benchmark, without inferred results."""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median


METRICS = (
    ("First decoded frame (ms; VLC only)", "first_frame_ms", 1),
    ("Verified prebuffer readiness (ms)", "startup_ms", 1),
    ("Post-start read stalls >=50 ms", "post_start_stalls", 1),
    ("Post-start read stall time (ms)", "post_start_stall_ms", 1),
    ("Post-start verified-data waiting (ms)", "post_start_wait_ms", 1),
    ("Player buffering episodes", "vlc_buffer_stalls", 1),
    ("Player buffering time (ms)", "vlc_buffer_ms", 1),
    ("Average verified forward buffer (s)", "average_forward_s", 1),
    ("Peak managed allocation (MiB)", "peak_allocated", 1024 * 1024),
    ("Sampled slot utilization (%)", "average_utilization", 0.01),
    ("Torrent rebuilds", "rebuilds", 1),
    ("Average rebuild duration (ms)", "average_rebuild_ms", 1),
    ("Maximum rebuild duration (ms)", "maximum_rebuild_ms", 1),
    ("Seek-request read recovery (ms)", "seek_read_ms", 1),
    ("Useful verified delivery (MiB/s)", "verified_bytes_per_s", 1024 * 1024),
)


def load(path):
    groups = defaultdict(list)
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            name = row.pop("workload").rsplit("-", 1)[0]
            groups[name].append({key: float(value) for key, value in row.items()})
    if not groups or any(len(rows) != 3 for rows in groups.values()):
        raise ValueError("Expected three complete repetitions of each workload")
    return groups


def render(before, after):
    if before.keys() != after.keys():
        raise ValueError("Before/after workload sets differ")
    lines = ["# Engine 2.0 measured comparison", "",
             "Three repetitions with real loopback peers and disk cache; VLC rows decode real video.",
             "Values are medians; maximum rebuild time is the largest across all runs.",
             "See [method, constraints and interpretation](../adaptive-engine-2.md).", ""]
    for workload in before:
        lines += [f"## {workload}", "", "| Metric | Baseline | Adaptive |", "|---|---:|---:|"]
        for label, key, divisor in METRICS:
            if key.startswith("vlc_") and workload != "vlc":
                continue
            if key == "seek_read_ms" and workload != "seeking":
                continue
            values = []
            for groups in (before, after):
                measure = max if key == "maximum_rebuild_ms" else median
                value = measure(row[key] for row in groups[workload]) / divisor
                values.append("N/A" if value < 0 else f"{value:,.3f}")
            lines.append(f"| {label} | {values[0]} | {values[1]} |")
        lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(render(load(args.before), load(args.after)) + "\n")


if __name__ == "__main__":
    main()
