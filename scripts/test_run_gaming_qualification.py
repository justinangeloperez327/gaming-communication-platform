"""End-to-end pipeline tests using synthetic data; not hardware qualification."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

p = Path(__file__).with_name("run-gaming-qualification.py")
spec = importlib.util.spec_from_file_location("gaming_qualification_pipeline", p)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class PipelineTests(unittest.TestCase):
    def make_captures(self, root):
        baseline, gcp, voice = [], [], []
        for i in range(3):
            a = root / f"base{i}.csv"
            b = root / f"gcp{i}.csv"
            c = root / f"voice{i}.txt"
            a.write_text("frame_ms\n" + "16\n" * 200)
            b.write_text("frame_ms\n" + "17\n" * 200)
            c.write_text('{"probe":"livekit_voice_session","cpu_percent_total_machine":0.8,"peak_private_bytes_sampled":104857600,"session_seconds":60,"memory_sample_count":29}\n')
            baseline.append(a)
            gcp.append(b)
            voice.append(c)
        return baseline, gcp, voice

    def test_pipeline_pass_and_artifacts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            baseline, gcp, voice = self.make_captures(root)
            out = root / "results"
            result = runner.execute(baseline, gcp, voice, "frame_ms", 0, (10, 2, 200, 10), out)
            self.assertEqual(result, 0)
            for artifact in ("benchmark.json", "qualification.json", "summary.md"):
                self.assertTrue((out / artifact).is_file())
            self.assertIn("PASS", (out / "summary.md").read_text())

    def test_regression_exit_code(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            baseline, gcp, voice = self.make_captures(root)
            out = root / "results"
            result = runner.execute(baseline, gcp, voice, "frame_ms", 0, (1, 2, 200, 1), out)
            self.assertEqual(result, 2)
            self.assertIn("REGRESSION", (out / "summary.md").read_text())

    def test_requires_three_runs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            baseline, gcp, voice = self.make_captures(root)
            with self.assertRaises(ValueError):
                runner.execute(baseline[:2], gcp[:2], voice[:2], "frame_ms", 0,
                               (10, 2, 200, 10), root / "results")

if __name__ == "__main__":
    unittest.main()
