"""Desktop CPU/RAM/attributed-GPU and QPC-aligned gameplay evidence analysis.

All origin/media-quality statements remain operator declarations. No collector
output or successful numerical gate authenticates a physical gaming session.
"""
import csv
import datetime as dt
import importlib.util
import math
import re
import statistics
from pathlib import Path

from benchmark_io import checked_file, digest, json_value

_spec = importlib.util.spec_from_file_location("desktop_frame_metrics", Path(__file__).with_name("compare-frame-times.py"))
_frame = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_frame)
SCENARIOS = {"baseline": (0, 0), "idle": (0, 0), "voice": (1, 0),
             "voice_publish": (1, 1), "voice_view": (1, 1), "voice_publish_view": (1, 2)}
REQUIRED_SCENARIOS = {"voice", "voice_publish", "voice_view"}
LIMITS = ("max_fps_loss_pct", "max_p99_increase_pct", "max_cpu_pct", "max_peak_private_mib",
          "max_memory_growth_mib_per_minute", "max_gpu_engine_pct")


def number(value, name, low=0, high=1e18, integral=False):
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not math.isfinite(value) or not low <= value <= high or
            (integral and not isinstance(value, int))):
        raise ValueError(f"Invalid {name}")
    return value


def text(value, name, maximum=256):
    if not isinstance(value, str) or not value.strip() or len(value) > maximum or any(ord(c) < 32 for c in value):
        raise ValueError(f"Invalid {name}")
    return value


def mapping(value):
    if not isinstance(value, dict):
        raise ValueError("Expected an observation object")
    return value


def utc(value):
    parsed = dt.datetime.fromisoformat(text(value, "UTC timestamp", 64).replace("Z", "+00:00"))
    if parsed.tzinfo is None or parsed.utcoffset() != dt.timedelta(0):
        raise ValueError("Expected an explicit UTC timestamp")
    return parsed


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def sha(value):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{64}", value):
        raise ValueError("Invalid executable SHA-256")
    return value


def read_capture(path, warmup, minimum_seconds):
    path = checked_file(path)
    with path.open(encoding="utf-8-sig") as source:
        records = []
        for index, line in enumerate(iter(lambda: source.readline(65538), "")):
            if index > 3603 or len(line) > 65536:
                raise ValueError("Capture record/storage bound exceeded")
            records.append(json_value(line))
    if len(records) < 5 or any(not isinstance(row, dict) for row in records):
        raise ValueError("Incomplete desktop capture")
    header, footer = records[0], records[-1]
    samples = records[1:-1]
    if (header.get("type") != "header" or (type(header.get("schema")) is not int or header["schema"] != 1) or
            header.get("collector") != "gcp-desktop-performance" or footer.get("type") != "end" or
            footer.get("complete") is not True or footer.get("samples") != len(samples)):
        raise ValueError("Missing successful capture completion")
    scenario = header.get("scenario")
    if scenario not in SCENARIOS:
        raise ValueError("Unknown desktop scenario")
    frequency = number(header.get("qpc_frequency"), "QPC frequency", 1, 10**12, True)
    interval = number(header.get("interval_seconds"), "sample interval", 1, 10, True)
    requested = number(header.get("requested_seconds"), "requested duration", 5, 3600, True)
    if not isinstance(header.get("skip_gpu"), bool):
        raise ValueError("Missing explicit GPU collection mode")
    started = utc(header.get("started_utc"))
    sha(header.get("boot_fingerprint"))
    hardware = header.get("hardware")
    if not isinstance(hardware, dict):
        raise ValueError("Missing hardware metadata")
    logical = number(hardware.get("logical_processors"), "logical processors", 1, 1024, True)
    number(hardware.get("ram_bytes"), "system RAM", 1, 2**60, True)
    if not isinstance(hardware.get("cpu"), list) or not hardware["cpu"]:
        raise ValueError("Missing CPU model")
    for item in hardware["cpu"]:
        text(item, "CPU model")
    if not isinstance(hardware.get("gpu"), list) or len(hardware["gpu"]) > 32:
        raise ValueError("Invalid GPU metadata")
    for item in hardware["gpu"]:
        mapping(item)
        text(item.get("name"), "GPU model"); text(item.get("driver"), "GPU driver")
    text(hardware.get("windows_build"), "Windows build")
    game = mapping(header.get("game", {}))
    game_id = number(game.get("pid"), "game PID", 1, 2**31-1, True)
    utc(game.get("start_utc")); sha(game.get("sha256")); text(game.get("name"), "game name")
    processes = header.get("processes")
    if not isinstance(processes, list) or len(processes) > 5:
        raise ValueError("Invalid process set")
    identities, hashes, gpu_required = {}, {}, set()
    counts = {"desktop": 0, "voice": 0, "screen": 0}
    for process in processes:
        mapping(process)
        process_id = number(process.get("pid"), "app PID", 1, 2**31-1, True)
        role = process.get("role")
        if role not in counts or process_id in identities or process_id == game_id:
            raise ValueError("Invalid/duplicate process role or PID")
        expected_name = {"desktop": "gcp-desktop.exe", "voice": "gcp-livekit-voice.exe", "screen": "gcp-livekit-screen.exe"}[role]
        if process.get("name", "").lower() != expected_name:
            raise ValueError("Unexpected executable name")
        utc(process.get("start_utc")); identities[process_id] = process["start_utc"]
        hashes.setdefault(role, set()).add(sha(process.get("sha256")))
        counts[role] += 1
        if role != "voice":
            gpu_required.add(process_id)
    expected = SCENARIOS[scenario]
    if counts != {"desktop": int(scenario != "baseline"), "voice": expected[0], "screen": expected[1]}:
        raise ValueError("Process roles do not match scenario")
    previous_qpc = None
    previous_cpu = {}
    totals = []
    gpu_series = []
    gpu_missing = []
    for index, sample in enumerate(samples):
        if sample.get("type") != "sample" or type(sample.get("index")) is not int or sample.get("index") != index:
            raise ValueError("Noncontiguous capture sample")
        qpc = number(sample.get("qpc"), "QPC sample", 1, 2**63-1, True)
        if previous_qpc is not None and (qpc <= previous_qpc or (qpc-previous_qpc)/frequency > 3*interval+2):
            raise ValueError("Nonmonotonic clock or excessive sampling gap")
        observed_game = mapping(sample.get("game", {}))
        if number(observed_game.get("pid"), "sample game PID", 1, 2**31-1, True) != game_id or observed_game.get("start_utc") != game["start_utc"]:
            raise ValueError("Game process restarted")
        observed = sample.get("processes")
        if not isinstance(observed, list) or len(observed) != len(identities):
            raise ValueError("App process set changed")
        cpu, private, working, seen = 0, 0, 0, set()
        for process in observed:
            mapping(process)
            process_id = number(process.get("pid"), "sample PID", 1, 2**31-1, True)
            if process_id in seen or process_id not in identities or process.get("start_utc") != identities[process_id]:
                raise ValueError("App process exited/restarted or PID was reused")
            seen.add(process_id)
            used = number(process.get("cpu_seconds"), "cumulative process CPU", 0, 1e12)
            if used < previous_cpu.get(process_id, used):
                raise ValueError("Process CPU clock moved backwards")
            previous_cpu[process_id] = used; cpu += used
            private += number(process.get("private_bytes"), "private bytes", 1, 2**60, True)
            working += number(process.get("working_set_bytes"), "working set", 1, 2**60, True)
            number(process.get("handles"), "handles", 0, 2**31-1, True)
            number(process.get("threads"), "threads", 1, 2**20, True)
        gpu = mapping(sample.get("gpu", {}))
        if gpu.get("status") not in {"ok", "no_instances", "unavailable", "not_requested", "not_applicable"} or not isinstance(gpu.get("engines"), list):
            raise ValueError("Invalid GPU observation")
        engines, gpu_pids, gpu_keys = {}, set(), set()
        if len(gpu["engines"]) > 128 or (gpu["status"] != "ok" and gpu["engines"]):
            raise ValueError("Invalid GPU rows/status")
        for row in gpu["engines"]:
            mapping(row)
            process_id = number(row.get("pid"), "GPU PID", 1, 2**31-1, True)
            adapter = text(row.get("adapter"), "GPU adapter", 96)
            engine = number(row.get("engine"), "GPU engine", 0, 1024, True)
            kind = text(row.get("kind"), "GPU engine type", 64)
            if process_id not in identities or (process_id, adapter, engine) in gpu_keys:
                raise ValueError("Foreign/duplicate GPU process-engine sample")
            gpu_keys.add((process_id, adapter, engine)); gpu_pids.add(process_id)
            key = (adapter, engine)
            if key in engines and engines[key][0] != kind:
                raise ValueError("Conflicting GPU engine labels")
            value = number(row.get("percent"), "GPU percentage", 0, 100)
            engines[key] = (kind, engines.get(key, (kind, 0))[1] + value)
        # Sum selected PIDs on the SAME physical engine, never different engines.
        if any(value > 100.1 for _, value in engines.values()):
            raise ValueError("PID-attributed physical-engine sum exceeds 100 percent")
        complete_gpu = scenario == "baseline" or (gpu["status"] == "ok" and bool(hardware["gpu"]) and gpu_required <= gpu_pids)
        gpu_missing.append(not complete_gpu)
        gpu_series.append(max((value for _, value in engines.values()), default=None) if complete_gpu else None)
        totals.append((qpc, cpu, private, working))
        previous_qpc = qpc
    if footer.get("qpc") != totals[-1][0]:
        raise ValueError("Capture end does not match final sample")
    duration = (totals[-1][0]-totals[0][0])/frequency
    if duration < requested - (3*interval+2) or duration > requested + 3*interval+5:
        raise ValueError("Capture duration disagrees with request")
    number(warmup, "warmup", 0, 600); number(minimum_seconds, "minimum window", 5, 3600)
    window = [i for i, row in enumerate(totals) if row[0] >= totals[0][0] + warmup*frequency]
    if len(window) < 3 or (totals[window[-1]][0]-totals[window[0]][0])/frequency < minimum_seconds:
        raise ValueError("Insufficient measured window after warmup")
    first, last = window[0], window[-1]
    elapsed = (totals[last][0]-totals[first][0])/frequency
    cpu_rates = []
    for index in range(first+1, last+1):
        seconds = (totals[index][0]-totals[index-1][0])/frequency
        cpu_rates.append(number(100*(totals[index][1]-totals[index-1][1])/(seconds*logical), "machine CPU percentage", 0, 100.1))
    x = [(totals[i][0]-totals[first][0])/frequency/60 for i in window]
    y = [totals[i][2]/1048576 for i in window]
    mean_x, mean_y = statistics.mean(x), statistics.mean(y)
    slope = sum((a-mean_x)*(b-mean_y) for a,b in zip(x,y))/sum((a-mean_x)**2 for a in x)
    gpu_values = [gpu_series[i] for i in window if gpu_series[i] is not None]
    observer = footer.get("observer", {})
    number(observer.get("cpu_percent_machine"), "observer CPU", 0, 100.1)
    number(observer.get("peak_private_bytes_sampled"), "observer memory", 1, 2**60, True)
    return {"header": header, "started": started, "interval": (totals[0][0], totals[-1][0]),
            "window": (totals[first][0], totals[last][0]), "seconds": elapsed,
            "binary_hashes": {role: sorted(values) for role,values in hashes.items()},
            "metrics": {"cpu_percent_machine": 100*(totals[last][1]-totals[first][1])/(elapsed*logical),
                        "p95_cpu_percent_machine": percentile(cpu_rates, .95),
                        "peak_private_mib_sampled": max(y),
                        "peak_working_set_mib_sampled": max(totals[i][3] for i in window)/1048576,
                        "private_growth_mib_per_minute": slope,
                        "peak_gpu_busiest_engine_pct": max(gpu_values) if gpu_values else None,
                        "gpu_coverage_complete": not any(gpu_missing[i] for i in window)},
            "observer": observer}


def read_frames(path, capture, swapchain):
    path = checked_file(path)
    text(swapchain, "swapchain", 80)
    durations, timestamps = [], []
    first, last = capture["window"]
    frequency = capture["header"]["qpc_frequency"]
    process_id = capture["header"]["game"]["pid"]
    excluded = 0
    with path.open(encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        fields = reader.fieldnames
        if not fields or len(fields) != len(set(fields)) or not {"ProcessID", "SwapChainAddress", "CPUStartQPC", "FrameTime"} <= set(fields):
            raise ValueError("PresentMon v2 CSV must include ProcessID,SwapChainAddress,CPUStartQPC,FrameTime; use --qpc_time")
        for count, row in enumerate(reader):
            if count > 2_000_000:
                raise ValueError("Too many frame rows")
            if row.get("ProcessID") != str(process_id) or row.get("SwapChainAddress") != swapchain:
                excluded += 1; continue
            try:
                stamp = int(row["CPUStartQPC"])
                milliseconds = float(row["FrameTime"])
            except (ValueError, TypeError) as error:
                raise ValueError("Invalid QPC/frame time") from error
            number(stamp, "frame QPC", 1, 2**63-1, True)
            if not first <= stamp <= last:
                excluded += 1; continue
            number(milliseconds, "frame milliseconds", 0.000001, 60000)
            if stamp + milliseconds*frequency/1000 > last:
                excluded += 1; continue
            if timestamps and stamp <= timestamps[-1]:
                raise ValueError("Duplicate/out-of-order game frame timestamp")
            if timestamps and abs((stamp-timestamps[-1])/frequency - durations[-1]/1000) > .001:
                raise ValueError("Missing, overlapping or misaligned game frames")
            timestamps.append(stamp); durations.append(milliseconds)
    result = _frame.summarize_frame_times(durations)
    span = (timestamps[-1]-timestamps[0])/frequency + durations[-1]/1000
    leading = (timestamps[0]-first)/frequency
    trailing = (last-timestamps[-1])/frequency - durations[-1]/1000
    if (leading > durations[0]/1000 + .001 or trailing > durations[-1]/1000 + .001 or
            abs(sum(durations)/1000 - span) > max(.005, len(durations)*.000001)):
        raise ValueError("Frame stream has gaps, overlapping frames or incomplete QPC-window coverage")
    result["frame_metric"] = "PresentMon v2 CPU frame-start interval (FrameTime)"
    result["time_domain"] = "Windows QPC"
    result["trimmed_edge_seconds"] = leading + trailing
    result["excluded_rows"] = excluded
    return result


def analyze(manifest, base):
    if not isinstance(manifest, dict) or (type(manifest.get("schema")) is not int or manifest["schema"] != 1):
        raise ValueError("Expected desktop evidence schema 1")
    kind = manifest.get("evidence_kind")
    if kind not in {"fixture", "operator_capture"}:
        raise ValueError("Declare evidence_kind fixture or operator_capture")
    for name in ("operator", "game_scene_settings", "frame_capture_version"):
        text(manifest.get(name), name, 2048)
    commit = manifest.get("build_commit")
    if not isinstance(commit, str) or not re.fullmatch("[0-9a-f]{40}", commit):
        raise ValueError("Supply the tested GCP build commit")
    warmup = number(manifest.get("warmup_seconds"), "warmup", 0, 600)
    minimum = number(manifest.get("minimum_window_seconds"), "minimum window", 5 if kind == "fixture" else 60, 3600)
    limits = manifest.get("limits", {})
    for key in LIMITS:
        number(limits.get(key), key, 0, 100 if key.endswith("pct") else 2**30)
    attestations = manifest.get("operator_attestations")
    if not isinstance(attestations, dict) or any(not isinstance(attestations.get(key), bool) for key in ("same_scene_settings", "media_quality_acceptable", "two_pc_media_observed")):
        raise ValueError("Explicit operator media/scene declarations are required")
    runs = manifest.get("runs")
    if not isinstance(runs, list) or not 3 <= len(runs) <= 60:
        raise ValueError("At least three paired runs required (maximum 60)")
    from benchmark_io import checked_file as input_file
    seen, content_seen, intervals, pairs, context, hashes, ordering = set(), set(), [], [], None, {}, {}
    input_records = []
    for run in runs:
        if not isinstance(run, dict) or run.get("scenario") not in SCENARIOS or run["scenario"] == "baseline":
            raise ValueError("Invalid run scenario")
        scenario = run["scenario"]
        loaded = {}
        for condition in ("baseline", "gcp"):
            item = run.get(condition, {})
            if not isinstance(item, dict):
                raise ValueError("Missing paired evidence")
            paths = []
            for key in ("resources", "frames"):
                name = text(item.get(key), key, 1024)
                relative = Path(name)
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("Capture paths must stay within the evidence directory")
                path = input_file(base/relative, seen)
                if not path.is_relative_to(base.resolve()):
                    raise ValueError("Capture symlink escapes evidence directory")
                content_hash = digest(path)
                if content_hash in content_seen:
                    raise ValueError("Duplicate capture contents reused")
                content_seen.add(content_hash); paths.append(path)
                input_records.append({"path": name, "sha256": content_hash, "bytes": path.stat().st_size})
            capture = read_capture(paths[0], warmup, minimum)
            header = capture["header"]
            if header["scenario"] != ("baseline" if condition == "baseline" else scenario):
                raise ValueError("Capture scenario does not match manifest")
            current_context = (header["boot_fingerprint"], header["hardware"], header["qpc_frequency"], header["game"]["sha256"], header["interval_seconds"], header["skip_gpu"])
            if context is not None and context != current_context:
                raise ValueError("Paired evidence must use one boot/hardware/game/observer configuration")
            context = current_context
            for role, values in capture["binary_hashes"].items():
                if role in hashes and hashes[role] != values:
                    raise ValueError("Application binaries changed between runs")
                hashes[role] = values
            intervals.append(capture["interval"])
            loaded[condition] = (capture, read_frames(paths[1], capture, item.get("swapchain")))
        baseline, before = loaded["baseline"]; app, after = loaded["gcp"]
        if abs(baseline["seconds"]-app["seconds"]) > max(2, .05*baseline["seconds"]):
            raise ValueError("Paired measured durations are not comparable")
        order = baseline["interval"][0] < app["interval"][0]
        ordering.setdefault(scenario, []).append(order)
        metrics = dict(app["metrics"])
        metrics.update({"average_fps_loss_pct": 100*(1-after["average_fps"]/before["average_fps"]),
                        "one_percent_low_loss_pct": 100*(1-after["one_percent_low_fps"]/before["one_percent_low_fps"]),
                        "p99_increase_pct": 100*(after["p99_frame_time_ms"]/before["p99_frame_time_ms"]-1)})
        checks = {"average_fps": metrics["average_fps_loss_pct"] <= limits["max_fps_loss_pct"],
                  "one_percent_low": metrics["one_percent_low_loss_pct"] <= limits["max_fps_loss_pct"],
                  "p99_frame_time": metrics["p99_increase_pct"] <= limits["max_p99_increase_pct"],
                  "cpu": metrics["cpu_percent_machine"] <= limits["max_cpu_pct"],
                  "cpu_p95": metrics["p95_cpu_percent_machine"] <= limits["max_cpu_pct"],
                  "private_memory": metrics["peak_private_mib_sampled"] <= limits["max_peak_private_mib"],
                  "memory_growth": metrics["private_growth_mib_per_minute"] <= limits["max_memory_growth_mib_per_minute"],
                  "gpu": (metrics["peak_gpu_busiest_engine_pct"] <= limits["max_gpu_engine_pct"]) if metrics["gpu_coverage_complete"] else None}
        pairs.append({"scenario": scenario, "metrics": metrics, "checks": checks, "baseline_frames": before,
                      "gcp_frames": after, "observer": app["observer"], "measured_seconds": app["seconds"]})
    intervals.sort()
    if any(left[1] >= right[0] for left,right in zip(intervals, intervals[1:])):
        raise ValueError("Capture windows overlap or were reused")
    for orders in ordering.values():
        if len(orders) < 3 or all(orders) or not any(orders):
            raise ValueError("Each scenario requires three pairs with both baseline-first and GCP-first order")
    for item in input_records:
        if digest(base/item["path"]) != item["sha256"]:
            raise ValueError("Input changed during analysis")
    regression = any(value is False for pair in pairs for value in pair["checks"].values())
    missing = any(value is None for pair in pairs for value in pair["checks"].values()) or not all(attestations.values())
    status = "regression" if regression else "incomplete" if missing else "limits_met"
    return {"schema": 1, "status": status, "input_files": input_records,
            "product_qualification_status": "pending_owner_acceptance" if kind == "operator_capture" and status == "limits_met" and REQUIRED_SCENARIOS <= ordering.keys() else "not_qualified", "evidence_kind": kind, "build_commit_declared": commit,
            "scope": sorted(ordering), "all_product_scenarios_present": REQUIRED_SCENARIOS <= ordering.keys(),
            "pairs": pairs, "limits": limits, "observed_binary_hashes": hashes,
            "operator_attestations_unverified": attestations, "scenario_labels_operator_declared": True, "physical_capture_authenticated": False,
            "physical_media_verified": False, "gaming_performance_verified_by_ci": False,
            "competitor_superiority_verified": False, "qualification_requires_owner_review": True}
