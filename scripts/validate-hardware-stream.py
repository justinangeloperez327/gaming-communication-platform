#!/usr/bin/env python3
"""Evaluate two actual GCP streaming diagnostic logs without inventing GPU results."""
import argparse
import csv
import json
import math
import sys
from pathlib import Path

SENDER = "hardware_h264_livekit"
RECEIVER = "livekit_decoded_frame_timing"


def nonnegative(obj, key, integral=False):
    value = obj.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or (
        not math.isfinite(value) or value < 0
    ):
        raise ValueError(f"Missing, negative or nonfinite {key}")
    if integral and (not isinstance(value, int) or value > 10**12):
        raise ValueError(f"Invalid integer count {key}")
    return value


def read_probe(path, kind):
    matches = []
    for line_number, line in enumerate(Path(path).read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip().startswith("{"):
            continue
        try:
            value = json.loads(line, parse_constant=lambda v: (_ for _ in ()).throw(ValueError(v)))
        except ValueError as err:
            raise ValueError(f"{path}:{line_number}: invalid JSON diagnostic") from err
        if isinstance(value, dict) and value.get("probe") == kind:
            matches.append(value)
    if len(matches) != 1:
        raise ValueError(f"{path}: expected exactly one {kind} diagnostic, found {len(matches)}")
    result = matches[0]
    if kind == SENDER:
        for key in ("packets_accepted", "packets_rejected", "accepted_keyframes",
                    "accepted_encoded_bytes"):
            nonnegative(result, key, True)
        nonnegative(result, "sender_observation_seconds")
    else:
        for key in ("decoded_frames", "interval_samples"):
            nonnegative(result, key, True)
        nonnegative(result, "frame_rate_since_first_callback")
    return result


def read_gpu_csv(path):
    """Read GPU VideoEncode counter values without assuming process attribution."""
    points = set()
    vals = []
    with Path(path).open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        expected = {"TimestampUtc", "GpuEnginePath", "UtilizationPercent"}
        if not reader.fieldnames or not expected.issubset(reader.fieldnames):
            raise ValueError("GPU CSV must contain TimestampUtc,GpuEnginePath,UtilizationPercent")
        for row in reader:
            if "engtype_videoencode" not in (row.get("GpuEnginePath") or "").lower():
                continue
            when = (row.get("TimestampUtc") or "").strip()
            if not when:
                raise ValueError("Empty VideoEncode timestamp")
            try:
                utilization = float(row.get("UtilizationPercent") or "")
            except ValueError as err:
                raise ValueError("Non-numeric VideoEncode utilization") from err
            if not math.isfinite(utilization) or not 0 <= utilization <= 100:
                raise ValueError("Invalid GPU VideoEncode utilization percentage")
            points.add(when)
            vals.append(utilization)
    return {
        "sample_rows": len(vals),
        "timepoints": len(points),
        "peak_percent": max(vals, default=0),
        "nonzero_rows": sum(value > 0 for value in vals),
        "activity_observed": len(points) >= 10 and any(value > 0 for value in vals),
        "process_attribution_verified": False,
    }


def evaluate(sender, receiver, gpu=None):
    checks = {
        "sdk_packets_accepted": sender["packets_accepted"] > 0,
        "sdk_no_packet_rejections": sender["packets_rejected"] == 0,
        "h264_keyframe_present": sender["accepted_keyframes"] > 0,
        "encoded_data_present": sender["accepted_encoded_bytes"] > 0,
        "sender_runtime_positive": sender["sender_observation_seconds"] > 0,
        "receiver_h264_track": receiver.get("track") == "h264",
        "receiver_decoded_frames": receiver["decoded_frames"] > 1,
        "receiver_intervals_recorded": receiver["interval_samples"] > 0,
        "receiver_fps_positive": receiver["frame_rate_since_first_callback"] > 0,
    }
    return {
        "schema_version": 1,
        "transport_checks": checks,
        "transport_observations_pass": all(checks.values()),
        "sender_accepted_packets": sender["packets_accepted"],
        "sender_keyframes": sender["accepted_keyframes"],
        "receiver_decoded_frames": receiver["decoded_frames"],
        "receiver_callback_fps": receiver["frame_rate_since_first_callback"],
        "gpu_videoencode_counters": gpu,
        "physical_gpu_verified": False,
        "gpu_process_attribution_verified": False,
        "same_room_and_time_verified": False,
        "end_to_end_latency_verified": False,
        "game_fps_impact_verified": False,
        "release_qualified": False,
        "manual_gates": [
            "Confirm both logs are from the same test room/time and different participants",
            "Visually check correct remote video decoding and compare source image",
            "Attribute Video Encode engine usage to the selected physical encoder",
            "Measure game frame times, CPU/RAM, and representative multi-minute stability",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sender", type=Path, required=True)
    parser.add_argument("--receiver", type=Path, required=True)
    parser.add_argument("--gpu-csv", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        sender = read_probe(args.sender, SENDER)
        receiver = read_probe(args.receiver, RECEIVER)
        gpu = read_gpu_csv(args.gpu_csv) if args.gpu_csv else None
        report = evaluate(sender, receiver, gpu)
        result = json.dumps(report, indent=2, allow_nan=False)
        if args.output:
            args.output.write_text(result + "\n", encoding="utf-8")
        print(result)
        # Success here ONLY indicates the supplied transport logs agree
        # with a minimal observed-playback callback gate, not hardware proof.
        return 0 if report["transport_observations_pass"] else 2
    except (OSError, ValueError, csv.Error) as error:
        print(f"Invalid stream evidence: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
