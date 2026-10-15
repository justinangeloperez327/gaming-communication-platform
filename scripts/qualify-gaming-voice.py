#!/usr/bin/env python3
"""Evaluate measured GCP voice/gaming benchmark against operator-supplied limits."""
import argparse
import json
import math
import sys
from pathlib import Path

def qualify(report, max_fps_loss_pct, max_cpu_pct, max_peak_mib, max_p99_increase_pct):
    for name, limit in (("fps_loss_pct", max_fps_loss_pct), ("cpu_pct", max_cpu_pct),
                        ("peak_mib", max_peak_mib), ("p99_increase_pct", max_p99_increase_pct)):
        if isinstance(limit, bool) or not isinstance(limit, (int, float)) or not math.isfinite(limit) or limit < 0:
            raise ValueError(f"Invalid limit: {name}")
    if report.get("measurement") != "exported_frame_time_csv" or report.get("condition") != "gaming_with_gcp_voice":
        raise ValueError("Expected real frame-time GCP voice report")
    if report.get("runs_per_condition", 0) < 3:
        raise ValueError("At least three paired runs required")
    voice = report.get("voice_process", {})
    if "median_peak_private_bytes_sampled" not in voice:\n        raise ValueError("Missing voice memory metric")\n    metrics = {
        "average_fps_change_percent": report.get("average_fps_change_percent"),
        "one_percent_low_fps_change_percent": report.get("one_percent_low_fps_change_percent"),
        "p99_frame_time_increase_percent": 100 * (
            report["streaming"]["p99_frame_time_ms"] /
            report["baseline"]["p99_frame_time_ms"] - 1),
        "voice_cpu_percent_total_machine": voice.get("median_cpu_percent_total_machine"),
        "voice_peak_memory_mib": voice["median_peak_private_bytes_sampled"] / 1048576,
    }
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
           for v in metrics.values()):
        raise ValueError("Invalid measurements")
    checks = {
        "average_fps": metrics["average_fps_change_percent"] >= -max_fps_loss_pct,
        "one_percent_low_fps": metrics["one_percent_low_fps_change_percent"] >= -max_fps_loss_pct,
        "p99_frame_time": metrics["p99_frame_time_increase_percent"] <= max_p99_increase_pct,
        "voice_cpu": metrics["voice_cpu_percent_total_machine"] <= max_cpu_pct,
        "voice_private_memory": metrics["voice_peak_memory_mib"] <= max_peak_mib,
    }
    return {"qualified": all(checks.values()), "checks": checks, "measurements": metrics,
            "limits": {"max_fps_loss_pct": max_fps_loss_pct, "max_cpu_pct": max_cpu_pct,
                       "max_peak_mib": max_peak_mib, "max_p99_increase_pct": max_p99_increase_pct},
            "physical_audio_verified": False, "competitor_superiority_verified": False}

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--max-fps-loss-pct", type=float, required=True)
    parser.add_argument("--max-cpu-pct", type=float, required=True)
    parser.add_argument("--max-peak-mib", type=float, required=True)
    parser.add_argument("--max-p99-increase-pct", type=float, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = json.loads(args.report.read_text(encoding="utf-8"))
        result = qualify(report, args.max_fps_loss_pct, args.max_cpu_pct,
                         args.max_peak_mib, args.max_p99_increase_pct)
        output = json.dumps(result, indent=2, allow_nan=False)
        if args.output: args.output.write_text(output + "\n", encoding="utf-8")
        print(output)
        return 0 if result["qualified"] else 2
    except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError) as error:
        print(f"Invalid benchmark qualification: {error}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
