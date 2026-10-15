#!/usr/bin/env python3
"""Run GCP benchmark and qualification together using local, real capture files."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

def execute(baseline, gcp, voice, column, warmup, limits, output_dir):
    if not baseline or len(baseline) != len(gcp) or len(gcp) != len(voice):
        raise ValueError("All conditions require equal, nonempty run counts")
    if len(baseline) < 3:
        raise ValueError("At least three paired runs are required")
    output_dir.mkdir(parents=True, exist_ok=True)
    benchmark = output_dir / "benchmark.json"
    qualification = output_dir / "qualification.json"
    base = Path(__file__).resolve().parent
    cmd = [sys.executable, str(base / "benchmark-gaming-voice.py"),
           "--baseline", *map(str, baseline), "--gcp", *map(str, gcp),
           "--voice-json", *map(str, voice), "--column", column,
           "--warmup-seconds", str(warmup), "--output", str(benchmark)]
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode:
        raise ValueError("Benchmark failed: " + result.stderr.strip()[:500])
    cmd = [sys.executable, str(base / "qualify-gaming-voice.py"),
           "--report", str(benchmark), "--max-fps-loss-pct", str(limits[0]),
           "--max-cpu-pct", str(limits[1]), "--max-peak-mib", str(limits[2]),
           "--max-p99-increase-pct", str(limits[3]), "--output", str(qualification)]
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode not in (0, 2):
        raise ValueError("Qualification failed: " + result.stderr.strip()[:500])
    summary = json.loads(qualification.read_text(encoding="utf-8"))
    markdown = ["# GCP measured gaming qualification", "",
                f"Result: **{'PASS' if summary['qualified'] else 'REGRESSION'}**", "",
                "| Check | Result |", "|---|---|"]
    markdown.extend(f"| {key} | {'PASS' if passed else 'FAIL'} |"
                    for key, passed in summary["checks"].items())
    markdown.extend(["", "Three or more captured paired runs required. Physical audio, GPU usage,",
                     "and competitor superiority are **not verified** by this report.", ""])
    (output_dir / "summary.md").write_text("\n".join(markdown), encoding="utf-8")
    return result.returncode

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", nargs="+", type=Path, required=True)
    parser.add_argument("--gcp", nargs="+", type=Path, required=True)
    parser.add_argument("--voice-json", nargs="+", type=Path, required=True)
    parser.add_argument("--column", required=True)
    parser.add_argument("--warmup-seconds", type=float, default=0)
    parser.add_argument("--max-fps-loss-pct", type=float, required=True)
    parser.add_argument("--max-cpu-pct", type=float, required=True)
    parser.add_argument("--max-peak-mib", type=float, required=True)
    parser.add_argument("--max-p99-increase-pct", type=float, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        status = execute(args.baseline, args.gcp, args.voice_json, args.column,
                         args.warmup_seconds,
                         (args.max_fps_loss_pct, args.max_cpu_pct, args.max_peak_mib,
                          args.max_p99_increase_pct), args.output_dir)
        print(f"Qualification outputs: {args.output_dir}")
        return status
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Invalid qualification pipeline: {error}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
