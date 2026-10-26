#!/usr/bin/env python3
"""Validate a human-authored benchmark provenance manifest; never certifies hardware."""
import argparse
import json
import math
import sys
from pathlib import Path
from benchmark_io import checked_file

FIELDS = ("hardware_cpu", "hardware_gpu", "ram_gib", "windows_build",
          "gpu_driver", "game", "game_version", "graphics_preset",
          "resolution", "frame_capture_tool", "frame_capture_version",
          "voice_mode", "operator", "capture_date_utc")
def validate(manifest, base):
    if not isinstance(manifest, dict): raise ValueError("Expected a JSON object")
    for field in FIELDS:
        v = manifest.get(field)
        if isinstance(v, bool) or (not isinstance(v, (str, int, float))) or not str(v).strip():
            raise ValueError(f"Missing evidence metadata: {field}")
    if isinstance(manifest["ram_gib"], bool) or not isinstance(manifest["ram_gib"], (int, float)) or not math.isfinite(manifest["ram_gib"]) or manifest["ram_gib"] <= 0:
        raise ValueError("Invalid RAM capacity")
    if manifest["voice_mode"] not in ("listen_only", "explicit_microphone"):
        raise ValueError("Invalid voice mode")
    pairs = manifest.get("runs")
    if not isinstance(pairs, list) or len(pairs) < 3:
        raise ValueError("At least three paired runs required")
    seen = set()
    for index, item in enumerate(pairs, 1):
        if not isinstance(item, dict): raise ValueError(f"Invalid run #{index}")
        for key in ("baseline_csv", "gcp_csv", "voice_log"):
            value = item.get(key)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"Missing {key} for run #{index}")
            checked_file(base / value, seen)
    return {"evidence_files_present": True, "run_count": len(pairs),
            "metadata_fields_present": True, "physical_capture_authenticated": False,
            "audio_quality_verified": False, "gpu_utilization_verified": False}

def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--output", type=Path)
    args=ap.parse_args(argv)
    try:
        result=validate(json.loads(args.manifest.read_text(encoding="utf-8")), args.manifest.resolve().parent)
        output=json.dumps(result,indent=2)
        if args.output: args.output.write_text(output+"\n",encoding="utf-8")
        print(output)
        return 0
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"Benchmark evidence invalid: {exc}",file=sys.stderr)
        return 1
if __name__=="__main__":sys.exit(main())
