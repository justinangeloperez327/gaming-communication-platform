"""Unit tests for real frame-time CSV comparison; no GPU required."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

PROGRAM = Path(__file__).resolve().parent / "compare-frame-times.py"
spec = importlib.util.spec_from_file_location("frame_comparison", PROGRAM)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FrameTimeComparisonTests(unittest.TestCase):
    def test_constant_frame_time(self):
        result = module.summarize_frame_times([20.0] * 100)
        self.assertEqual(result["average_fps"], 50.0)
        self.assertEqual(result["one_percent_low_fps"], 50.0)
        self.assertEqual(result["p99_frame_time_ms"], 20.0)

    def test_one_percent_slow_frame(self):
        result = module.summarize_frame_times([10.0] * 99 + [100.0])
        self.assertAlmostEqual(result["one_percent_low_fps"], 10.0)
        self.assertGreater(result["average_fps"], 90)

    def test_rejects_short_or_invalid(self):
        with self.assertRaises(ValueError):
            module.summarize_frame_times([16.0] * 99)
        with self.assertRaises(ValueError):
            module.summarize_frame_times([16.0] * 99 + [float("nan")])

    def test_real_csv_and_regression_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            baseline = root / "before.csv"
            streaming = root / "during.csv"
            baseline.write_text(
                "MsBetweenPresents\n" + "16\n" * 200, encoding="utf-8")
            streaming.write_text(
                "MsBetweenPresents\n" + "20\n" * 200, encoding="utf-8")
            result = module.evaluate([baseline], [streaming],
                                     "MsBetweenPresents", 0)
            self.assertAlmostEqual(result["average_fps_change_percent"], -20.0)
            self.assertAlmostEqual(result["one_percent_low_fps_change_percent"], -20.0)
            self.assertEqual(module.main([
                "--baseline", str(baseline),
                "--streaming", str(streaming),
                "--column", "MsBetweenPresents",
                "--max-regression-pct", "5",
                "--output", str(root / "result.json")]), 2)
            self.assertTrue((root / "result.json").exists())

    def test_rejects_non_finite_csv(self):
        with tempfile.TemporaryDirectory() as folder:
            csvfile = Path(folder) / "bad.csv"
            csvfile.write_text("time\n" + "16\n" * 99 + "inf\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                module.load_csv(csvfile, "time", 0)


if __name__ == "__main__":
    unittest.main()
