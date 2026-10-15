#!/usr/bin/env python3
"""Paired GCP gaming benchmark: frame-time exports plus native voice JSON metrics."""
import argparse
import json
import math
import statistics
import sys
from pathlib import Path
import importlib.util

spec = importlib.util.spec_from_file_location("gcp_frame_times", Path(__file__).with_name("compare-frame-times.py"))
frame = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frame)

def load_voice(path):
    # Native CLI prints JSON probe records alongside status/events; accept exactly one final voice record.
    records = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if len(line) > 16384:
            raise ValueError("Oversized telemetry line")
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and value.get("probe") == "livekit_voice_session":
            records.append(value)
    if len(records) != 1:
        raise ValueError(f"{path}: expected exactly one livekit_voice_session JSON record")
    record = records[0]
    keys = ("cpu_percent_total_machine", "peak_private_bytes_sampled", "session_seconds")
    for key in keys:
        number = record.get(key)
        if isinstance(number, bool) or not isinstance(number, (int, float)) or not math.isfinite(number) or number < 0:
            raise ValueError(f"{path}: invalid {key}")
    return {key: record[key] for key in keys}

def report(baseline, voice, telemetry, column, warmup):
    if not baseline or len(baseline) != len(voice) or len(voice) != len(telemetry):
        raise ValueError("An equal nonzero number of baseline, GCP and voice telemetry files is required")
    if len(set(map(str, baseline + voice + telemetry))) != 3 * len(baseline):
        raise ValueError("Each baseline, GCP and voice capture must be a distinct file")
    frames = frame.evaluate(baseline, voice, column, warmup)
    stats = [load_voice(path) for path in telemetry]
    if any(item["session_seconds"] <= 0 for item in stats):
        raise ValueError("Voice telemetry must represent a positive-duration session")
    frames["condition"] = "gaming_with_gcp_voice"
    frames["voice_process"] = {
        "median_cpu_percent_total_machine": statistics.median(x["cpu_percent_total_machine"] for x in stats),
        "median_peak_private_bytes_sampled": statistics.median(x["peak_private_bytes_sampled"] for x in stats),
        "run_metrics": stats,
    }
    frames["comparisons_to_other_apps_performed"] = False
    frames["physical_audio_verified"] = False
    return frames

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, nargs="+", required=True)
    parser.add_argument("--gcp", type=Path, nargs="+", required=True)
    parser.add_argument("--voice-json", type=Path, nargs="+", required=True)
    parser.add_argument("--column", required=True)
    parser.add_argument("--warmup-seconds", type=float, default=0.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.warmup_seconds) or args.warmup_seconds < 0:
            raise ValueError("Invalid warmup duration")
        value = report(args.baseline, args.gcp, args.voice_json, args.column, args.warmup_seconds)
        output = json.dumps(value, indent=2, allow_nan=False)
        if args.output:
            args.output.write_text(output + "\n", encoding="utf-8")
        print(output)
        return 0
    except (ValueError, OSError) as error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
