#!/usr/bin/env python3
"""Analyze QPC-aligned desktop/game captures; never certifies physical media."""
import argparse
import csv
import json
from pathlib import Path
import sys
import tempfile

from benchmark_io import checked_file, json_value
from desktop_performance import LIMITS, REQUIRED_SCENARIOS, analyze


def template(scenarios):
    runs = []
    for scenario in scenarios:
        for index in range(1, 4):
            runs.append({"scenario": scenario, **{condition: {
                "resources": f"{scenario}/{index}/{condition}.jsonl",
                "frames": f"{scenario}/{index}/{condition}.csv", "swapchain": "RECORD_EXACT_SWAPCHAIN"}
                for condition in ("baseline", "gcp")}})
    return {"schema": 1, "evidence_kind": "operator_capture", "build_commit": "RECORD_40_HEX_COMMIT",
            "operator": "RECORD_OPERATOR", "game_scene_settings": "RECORD_SCENE_RESOLUTION_PRESET_FPS_CAP_POWER_MODE",
            "frame_capture_version": "RECORD_PRESENTMON_VERSION", "warmup_seconds": 30,
            "minimum_window_seconds": 60, "limits": {key: None for key in LIMITS},
            "operator_attestations": {"same_scene_settings": False, "media_quality_acceptable": False,
                                      "two_pc_media_observed": False}, "runs": runs}


def execute(manifest_path, output_directory):
    manifest_path = checked_file(manifest_path)
    if manifest_path.stat().st_size > 131072:
        raise ValueError("Manifest is too large")
    result = analyze(json_value(manifest_path.read_text(encoding="utf-8-sig")), manifest_path.parent)
    # No old PASS summary may survive a failed run. Publish a new directory only
    # after the full analysis succeeds; refuse existing files/directories.
    output_directory = Path(output_directory).resolve()
    if output_directory.exists():
        raise ValueError("Choose a new output directory; old evidence is never overwritten")
    if output_directory == manifest_path.parent or output_directory in manifest_path.parents:
        raise ValueError("Output cannot replace the input directory")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    rows = ["# Desktop gaming evidence analysis", "",
            f"Numeric result: **{result['status']}** · evidence kind: **{result['evidence_kind']}**", "",
            "Physical capture, media quality and gaming performance are not authenticated by this tool or CI.", "",
            "| Scenario/pair | FPS loss % | 1% low loss % | CPU machine % | Peak private MiB | Growth MiB/min | Busiest GPU engine % |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for index, pair in enumerate(result["pairs"], 1):
        metrics = pair["metrics"]
        gpu = metrics["peak_gpu_busiest_engine_pct"]
        gpu_text = f"{gpu:.3f}" if gpu is not None else "missing"
        rows.append(f"| {pair['scenario']}/{index} | {metrics['average_fps_loss_pct']:.3f} | "
                    f"{metrics['one_percent_low_loss_pct']:.3f} | {metrics['cpu_percent_machine']:.3f} | "
                    f"{metrics['peak_private_mib_sampled']:.3f} | {metrics['private_growth_mib_per_minute']:.3f} | {gpu_text} |")
    rows += ["", "All per-pair checks must meet the supplied limits; medians cannot hide a bad run.",
             "GPU usage sums selected PIDs on the same physical engine and takes the busiest engine, not a sum across engines.",
             "Missing GPU observations or unconfirmed operator media/scene declarations leave the analysis incomplete.",
             "Required product scenarios present: " + str(result["all_product_scenarios_present"]),
             "Owner review, physical two-PC acceptance and actual gaming captures remain separate.", ""]
    with tempfile.TemporaryDirectory(prefix=".gcp-analysis-", dir=output_directory.parent) as staging:
        stage = Path(staging)/"result"; stage.mkdir()
        (stage/"report.json").write_text(json.dumps(result, indent=2, allow_nan=False)+"\n", encoding="utf-8")
        (stage/"summary.md").write_text("\n".join(rows), encoding="utf-8")
        # Recheck immediately before rename. No destructive replacement flag.
        if output_directory.exists():
            raise ValueError("Output appeared during analysis")
        stage.rename(output_directory)
    return {"limits_met": 0, "regression": 2, "incomplete": 3}[result["status"]]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--init", type=Path, help="Create an unfilled operator manifest, never evidence")
    parser.add_argument("--scenario", action="append", choices=sorted(REQUIRED_SCENARIOS | {"voice_publish_view", "idle"}))
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.init:
            if args.manifest or args.output_dir:
                raise ValueError("Use --init separately from analysis")
            args.init.parent.mkdir(parents=True, exist_ok=True)
            with args.init.open("x", encoding="utf-8") as output:
                json.dump(template(args.scenario or sorted(REQUIRED_SCENARIOS)), output, indent=2, allow_nan=False)
                output.write("\n")
            print("Created an unfilled manifest. Supply actual captures, explicit limits and honest operator declarations.")
            return 0
        if not args.manifest or not args.output_dir or args.scenario:
            raise ValueError("Analysis requires --manifest and --output-dir")
        status = execute(args.manifest, args.output_dir)
        print(f"Analysis saved to {args.output_dir}; no physical qualification is implied.")
        return status
    except (ValueError, OSError, TypeError, KeyError, AttributeError, RecursionError, csv.Error) as error:
        print(f"Desktop evidence invalid: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
