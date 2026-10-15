"""Manifest validation tests; fixtures do not establish physical hardware provenance."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

script = Path(__file__).with_name("validate-benchmark-evidence.py")
spec = importlib.util.spec_from_file_location("evidence", script)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

def fixture(root):
    data = {name: "recorded-value" for name in mod.FIELDS}
    data["ram_gib"] = 32
    data["voice_mode"] = "listen_only"
    data["runs"] = []
    for i in range(3):
        item = {}
        for field in ("baseline_csv", "gcp_csv", "voice_log"):
            filename = f"{field}_{i}.txt"
            (root / filename).write_text("fake fixture")
            item[field] = filename
        data["runs"].append(item)
    return data

class EvidenceTests(unittest.TestCase):
    def test_complete_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = mod.validate(fixture(root), root)
            self.assertEqual(result["run_count"], 3)
            self.assertFalse(result["physical_capture_authenticated"])

    def test_missing_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = fixture(root)
            (root / data["runs"][0]["voice_log"]).unlink()
            with self.assertRaises(ValueError):
                mod.validate(data, root)

    def test_duplicate_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = fixture(root)
            data["runs"][1]["baseline_csv"] = data["runs"][0]["baseline_csv"]
            with self.assertRaises(ValueError):
                mod.validate(data, root)

    def test_invalid_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = fixture(root)
            data["voice_mode"] = "always_on"
            with self.assertRaises(ValueError):
                mod.validate(data, root)

if __name__ == "__main__":
    unittest.main()
