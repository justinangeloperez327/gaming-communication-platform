#!/usr/bin/env python3
"""Compare repeatable exported frame-time CSV runs; standard library only.

CSV rows are frame durations in milliseconds, NOT sampled FPS. Tools such as
PresentMon can export frame presentation durations. Specify the EXACT name
of the column containing frame times via --column.
"""
import argparse
import csv
import json
import math
import statistics
import sys
from pathlib import Path


def summarize_frame_times(frame_times):
    """Time-weighted average and reciprocal of mean worst 1% frame durations."""
    if len(frame_times) < 100:
        raise ValueError("At least 100 valid frame-time rows are required per run")
    if not all(math.isfinite(t) and t > 0 for t in frame_times):
        raise ValueError("Frame times must be finite positive milliseconds")
    slow_count = max(1, math.ceil(len(frame_times) / 100))
    slowest = sorted(frame_times, reverse=True)[:slow_count]
    return {
        "frames": len(frame_times),
        "average_fps": 1000.0 / statistics.mean(frame_times),
        "one_percent_low_fps": 1000.0 / statistics.mean(slowest),
        "p99_frame_time_ms": sorted(frame_times)[
            math.ceil(0.99 * len(frame_times)) - 1
        ],
    }


def load_csv(path, column, warmup_seconds):
    durations = []
    with Path(path).open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None or column not in reader.fieldnames:
            raise ValueError(f"{path}: missing requested frame-time column {column!r}")
        elapsed = 0.0
        for row_number, row in enumerate(reader, start=2):
            raw = row.get(column)
            if raw is None or not raw.strip():
                raise ValueError(f"{path}:{row_number}: empty frame time")
            try:
                frame_ms = float(raw)
            except ValueError as error:
                raise ValueError(
                    f"{path}:{row_number}: invalid frame time {raw!r}"
                ) from error
            if not math.isfinite(frame_ms) or frame_ms <= 0:
                raise ValueError(f"{path}:{row_number}: nonpositive/NaN frame time")
            elapsed += frame_ms / 1000.0
            if elapsed > warmup_seconds:
                durations.append(frame_ms)
    return summarize_frame_times(durations)


def median_metrics(runs):
    return {
        key: statistics.median(x[key] for x in runs)
        for key in ("average_fps", "one_percent_low_fps", "p99_frame_time_ms")
    }


def evaluate(baseline_files, streaming_files, column, warmup_seconds):
    if len(baseline_files) != len(streaming_files):
        raise ValueError("Pass an equal number of baseline and streaming CSV runs")
    baseline_runs = [load_csv(p, column, warmup_seconds) for p in baseline_files]
    streaming_runs = [load_csv(p, column, warmup_seconds) for p in streaming_files]
    baseline = median_metrics(baseline_runs)
    streaming = median_metrics(streaming_runs)
    return {
        "measurement": "exported_frame_time_csv",
        "column": column,
        "warmup_seconds_excluded": warmup_seconds,
        "runs_per_condition": len(baseline_runs),
        "baseline": baseline,
        "streaming": streaming,
        "average_fps_change_percent": 100.0 * (
            streaming["average_fps"] / baseline["average_fps"] - 1.0
        ),
        "one_percent_low_fps_change_percent": 100.0 * (
            streaming["one_percent_low_fps"] / baseline["one_percent_low_fps"] - 1.0
        ),
        "baseline_runs": baseline_runs,
        "streaming_runs": streaming_runs,
        "gpu_hardware_utilization_measured": False,
        "results_automatically_collected": False,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", nargs="+", required=True, type=Path)
    parser.add_argument("--streaming", nargs="+", required=True, type=Path)
    parser.add_argument("--column", required=True, help="Frame-time field in milliseconds")
    parser.add_argument("--warmup-seconds", type=float, default=0)
    parser.add_argument("--max-regression-pct", type=float, default=None,
                        help="Optional gate for BOTH average and 1%%-low FPS")
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.warmup_seconds) or args.warmup_seconds < 0:
            raise ValueError("Warmup duration must be finite and nonnegative")
        if args.max_regression_pct is not None and (
            not math.isfinite(args.max_regression_pct)
            or args.max_regression_pct < 0 or args.max_regression_pct > 100
        ):
            raise ValueError("Max regression must be between 0 and 100 percent")
        report = evaluate(args.baseline, args.streaming, args.column,
                          args.warmup_seconds)
        if args.max_regression_pct is not None:
            report["max_regression_pct"] = args.max_regression_pct
            report["within_target"] = (
                report["average_fps_change_percent"] >= -args.max_regression_pct
                and report["one_percent_low_fps_change_percent"] >= -args.max_regression_pct
            )
        output = json.dumps(report, indent=2, allow_nan=False)
        if args.output:
            args.output.write_text(output + "\n", encoding="utf-8")
        print(output)
        return 2 if report.get("within_target") is False else 0
    except (OSError, ValueError, csv.Error) as error:
        print(f"Cannot evaluate game performance: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
