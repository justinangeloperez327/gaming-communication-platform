"""Offline tests of combined voice and frame-time benchmark."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

script = Path(__file__).with_name("benchmark-gaming-voice.py")
spec = importlib.util.spec_from_file_location("gcp_game_bench", script)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)

class Tests(unittest.TestCase):
    def test_paired_reports(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            base, gaming, voice = [p / s for s in ("baseline.csv", "gcp.csv", "voice.txt")]
            base.write_text("frame_ms\n" + "16\n"*200)
            gaming.write_text("frame_ms\n" + "20\n"*200)
            voice.write_text('GCP_VOICE_EVENT:connected\n' + json.dumps({
                "probe": "livekit_voice_session",
                "cpu_percent_total_machine": 0.8,
                "peak_private_bytes_sampled": 104857600,
                "session_seconds": 60.0}) + "\n")
            result = benchmark.report([base], [gaming], [voice], "frame_ms", 0)
            self.assertAlmostEqual(result["average_fps_change_percent"], -20)
            self.assertEqual(result["voice_process"]["median_peak_private_bytes_sampled"], 104857600)
    def test_rejects_missing_telemetry(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/"empty.txt"
            p.write_text("GCP_VOICE_EVENT:connected\n")
            with self.assertRaises(ValueError):
                benchmark.load_voice(p)
    def test_rejects_bad_metrics(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/"bad.txt"
            p.write_text('{"probe":"livekit_voice_session","cpu_percent_total_machine":-1,"peak_private_bytes_sampled":10,"session_seconds":30}\n')
            with self.assertRaises(ValueError):
                benchmark.load_voice(p)

if __name__ == "__main__":
    unittest.main()
