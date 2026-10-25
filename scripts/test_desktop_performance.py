"""Synthetic evidence only: these tests never establish physical gaming results."""
import copy
import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

import desktop_performance as perf
from benchmark_io import distinct_files

_spec = importlib.util.spec_from_file_location("desktop_qualifier", Path(__file__).with_name("qualify-desktop-performance.py"))
cli = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(cli)
FREQUENCY = 1_000_000
ADAPTER = "0x00000000_0x00000123_phys_0"


def capture(scenario, start, seconds=65):
    definitions = []
    if scenario != "baseline":
        definitions = [(101, "desktop", "gcp-desktop.exe"), (102, "voice", "gcp-livekit-voice.exe")]
        if "publish" in scenario or "view" in scenario:
            definitions.append((103, "screen", "gcp-livekit-screen.exe"))
    born = "2026-10-25T01:00:00+00:00"
    processes = [{"pid": pid, "role": role, "name": name, "start_utc": born, "sha256": {"desktop":"d", "voice":"e", "screen":"f"}[role]*64}
                 for pid,role,name in definitions]
    game = {"pid": 9000, "start_utc": born, "name": "fixture_game", "sha256": "a"*64}
    header = {"type": "header", "schema": 1, "collector": "gcp-desktop-performance", "scenario": scenario,
              "started_utc": (dt.datetime(2026,10,25,tzinfo=dt.timezone.utc)+dt.timedelta(seconds=start)).isoformat(),
              "qpc_frequency": FREQUENCY, "boot_fingerprint": "b"*64, "interval_seconds": 1,
              "requested_seconds": seconds, "skip_gpu": False,
              "hardware": {"cpu": ["fixture CPU"], "gpu": [{"name":"fixture GPU", "driver":"fixture driver"}],
                           "logical_processors": 8, "ram_bytes": 16*1024**3, "windows_build":"fixture"},
              "game": game, "processes": processes}
    rows = [header]
    for index in range(seconds+1):
        observations = [{"pid": p["pid"], "start_utc": born, "cpu_seconds": 10+index*.08,
                         "private_bytes": 50*1024**2, "working_set_bytes": 60*1024**2, "handles": 100, "threads": 3} for p in processes]
        engines = [{"pid":101, "adapter":ADAPTER, "engine":0, "kind":"3d", "percent":10}] if processes else []
        if len(processes) == 3:
            engines += [{"pid":103, "adapter":ADAPTER, "engine":0, "kind":"3d", "percent":15},
                        {"pid":103, "adapter":ADAPTER, "engine":1, "kind":"videoencode", "percent":30}]
        rows.append({"type":"sample", "index":index, "qpc":int((start+index)*FREQUENCY), "game":{"pid":9000,"start_utc":born},
                     "processes":observations, "gpu":{"status":"ok" if processes else "not_applicable", "engines":engines}})
    rows.append({"type":"end", "complete":True, "samples":seconds+1,"qpc":int((start+seconds)*FREQUENCY),
                 "observer":{"cpu_percent_machine":.2,"peak_private_bytes_sampled":40*1024**2}})
    return rows


def save_capture(path, records):
    path.write_text("\n".join(json.dumps(row, allow_nan=False) for row in records)+"\n", encoding="utf-8")


def save_frames(path, start, frame_ms=20):
    rows = ["ProcessID,SwapChainAddress,CPUStartQPC,FrameTime"]
    for index in range(int(65000/frame_ms)+1):
        rows.append(f"9000,0x123,{int((start+index*frame_ms/1000)*FREQUENCY)},{frame_ms}")
    path.write_text("\n".join(rows)+"\n", encoding="utf-8")


def fixture(root, scenario="voice"):
    data = cli.template([scenario]); data.update(evidence_kind="fixture", build_commit="c"*40, operator="test fixture",
        game_scene_settings="synthetic-only", frame_capture_version="fixture", warmup_seconds=0, minimum_window_seconds=60)
    data["limits"] = dict(zip(perf.LIMITS, (5, 10, 5, 200, 1, 40)))
    data["operator_attestations"] = {key:True for key in data["operator_attestations"]}
    for index, run in enumerate(data["runs"]):
        base_start = 1000+index*300
        starts = {"baseline":base_start, "gcp":base_start+100} if index != 1 else {"baseline":base_start+100, "gcp":base_start}
        for condition, start in starts.items():
            item = run[condition]; resource = root/item["resources"]; resource.parent.mkdir(parents=True,exist_ok=True)
            save_capture(resource, capture("baseline" if condition=="baseline" else scenario, start))
            save_frames(root/item["frames"], start, 20 if condition=="baseline" else 20.4)
            item["swapchain"] = "0x123"
    return data


class DesktopPerformanceTests(unittest.TestCase):
    def test_full_fixture_pipeline_is_never_physical_qualification(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); data = fixture(root)
            manifest = root/"manifest.json"; manifest.write_text(json.dumps(data))
            self.assertEqual(cli.execute(manifest, root/"analysis"), 0)
            report = json.loads((root/"analysis/report.json").read_text())
            self.assertEqual(report["evidence_kind"], "fixture")
            self.assertFalse(report["physical_capture_authenticated"])
            self.assertFalse(report["physical_media_verified"])
            self.assertFalse(report["all_product_scenarios_present"])
            self.assertEqual(len(report["input_files"]), 12)
            self.assertAlmostEqual(report["pairs"][0]["metrics"]["cpu_percent_machine"], 2)
            with self.assertRaises(ValueError): cli.execute(manifest, root/"analysis")

    def test_each_pair_gates_regression_not_just_medians(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); data=fixture(root)
            save_frames(root/data["runs"][2]["gcp"]["frames"], 1700, 30)
            result=perf.analyze(data,root)
            self.assertEqual(result["status"],"regression")
            self.assertFalse(result["pairs"][2]["checks"]["average_fps"])

    def test_gpu_sums_pids_on_same_engine_not_different_engines(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"capture.jsonl";save_capture(path,capture("voice_publish",1000))
            report=perf.read_capture(path,0,60)
            self.assertEqual(report["metrics"]["peak_gpu_busiest_engine_pct"],30)
            self.assertTrue(report["metrics"]["gpu_coverage_complete"])

    def test_missing_gpu_is_incomplete_not_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root)
            path=root/data["runs"][0]["gcp"]["resources"]
            records=[json.loads(line) for line in path.read_text().splitlines()]
            records[10]["gpu"]={"status":"unavailable","engines":[]};save_capture(path,records)
            result=perf.analyze(data,root)
            self.assertEqual(result["status"],"incomplete")
            self.assertIsNone(result["pairs"][0]["checks"]["gpu"])

    def test_unconfirmed_quality_stays_incomplete(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root);data["operator_attestations"]["media_quality_acceptable"]=False
            self.assertEqual(perf.analyze(data,root)["status"],"incomplete")

    def test_process_and_clock_corruption_is_rejected(self):
        changes = [lambda r:r[4]["processes"][0].update(start_utc="2026-10-25T02:00:00Z"),
                   lambda r:r[4]["processes"].pop(),
                   lambda r:r[4]["processes"][0].update(cpu_seconds=0),
                   lambda r:r[4].update(qpc=r[3]["qpc"]),
                   lambda r:r[4]["game"].update(pid=2),
                   lambda r:r[-1].update(complete=False),
                   lambda r:r[-1].update(samples=1),
                   lambda r:r[0].update(qpc_frequency=True),
                   lambda r:r[3]["gpu"]["engines"].append(copy.deepcopy(r[3]["gpu"]["engines"][0])),
                   lambda r:r[3]["gpu"]["engines"][0].update(pid=555),
                   lambda r:r[3]["gpu"]["engines"][0].update(percent=101)]
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"capture.jsonl"
            for change in changes:
                with self.subTest(change=change):
                    records=capture("voice",1000);change(records);save_capture(path,records)
                    with self.assertRaises(ValueError):perf.read_capture(path,0,60)

    def test_truncated_and_excessive_gap_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"capture.jsonl"; records=capture("voice",1000)
            save_capture(path,records[:-1])
            with self.assertRaises(ValueError):perf.read_capture(path,0,60)
            records=capture("voice",1000);records[5]["qpc"]+=10*FREQUENCY;save_capture(path,records)
            with self.assertRaises(ValueError):perf.read_capture(path,0,60)

    def test_frame_pid_swapchain_qpc_and_coverage(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);path=root/"capture.jsonl";save_capture(path,capture("voice",1000))
            sample=perf.read_capture(path,0,60);frames=root/"frames.csv";save_frames(frames,1000)
            with self.assertRaises(ValueError):perf.read_frames(frames,sample,"other_swapchain")
            save_frames(frames,2000)
            with self.assertRaises(ValueError):perf.read_frames(frames,sample,"0x123")
            save_frames(frames,1000);lines=frames.read_text().splitlines();lines.pop(10)
            lines.extend(["10000,0xabc,10,20"]*10);frames.write_text("\n".join(lines))
            with self.assertRaises(ValueError):perf.read_frames(frames,sample,"0x123")
            frames.write_text("\n".join(lines[:1000]))
            with self.assertRaises(ValueError):perf.read_frames(frames,sample,"0x123")

    def test_warmup_short_window_and_boolean_budget_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root);data["warmup_seconds"]=30
            with self.assertRaises(ValueError):perf.analyze(data,root)
            data["warmup_seconds"]=0;data["limits"]["max_cpu_pct"]=True
            with self.assertRaises(ValueError):perf.analyze(data,root)

    def test_pair_order_overlap_durations_and_build_mismatch(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root)
            # Replacing a resource with another pair's path must not pass as a new run.
            data["runs"][1]["gcp"]=data["runs"][0]["gcp"]
            with self.assertRaises(ValueError):perf.analyze(data,root)
            data=fixture(root)
            path=root/data["runs"][1]["gcp"]["resources"]
            records=[json.loads(line) for line in path.read_text().splitlines()]
            records[0]["processes"][0]["sha256"]="9"*64;save_capture(path,records)
            with self.assertRaises(ValueError):perf.analyze(data,root)

    def test_all_baseline_first_and_overlapping_windows_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root)
            run=data["runs"][1]
            save_capture(root/run["baseline"]["resources"],capture("baseline",1300))
            save_frames(root/run["baseline"]["frames"],1300)
            save_capture(root/run["gcp"]["resources"],capture("voice",1400))
            save_frames(root/run["gcp"]["frames"],1400,20.4)
            with self.assertRaises(ValueError):perf.analyze(data,root)
            data=fixture(root)
            save_capture(root/data["runs"][0]["gcp"]["resources"],capture("voice",1000))
            save_frames(root/data["runs"][0]["gcp"]["frames"],1000,20.4)
            with self.assertRaises(ValueError):perf.analyze(data,root)

    def test_changed_boot_and_unmatched_duration_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=fixture(root)
            path=root/data["runs"][0]["gcp"]["resources"]
            records=capture("voice",1100);records[0]["boot_fingerprint"]="1"*64;save_capture(path,records)
            with self.assertRaises(ValueError):perf.analyze(data,root)
            records=capture("voice",1100,seconds=60);save_capture(path,records)
            with self.assertRaises(ValueError):perf.analyze(data,root)

    def test_aliases_and_hardlinks_are_not_distinct_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);path=root/"a";path.write_text("fixture")
            with self.assertRaises(ValueError):distinct_files([path,root/"."/"a"])
            alias=root/"b";os.link(path,alias)
            with self.assertRaises(ValueError):distinct_files([path,alias])

    def test_init_is_unfilled_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"manifest.json"
            self.assertEqual(cli.main(["--init",str(path),"--scenario","voice"]),0)
            data=json.loads(path.read_text());self.assertTrue(all(value is None for value in data["limits"].values()))
            self.assertEqual(cli.main(["--init",str(path)]),1)
            self.assertEqual(cli.main(["--manifest",str(path),"--output-dir",str(Path(folder)/"results")]),1)


if __name__=="__main__":unittest.main()
