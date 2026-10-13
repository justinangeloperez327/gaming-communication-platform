"""No-GPU tests for GCP hardware streaming log validation."""
import importlib.util
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

FILE = Path(__file__).resolve().parent / "validate-hardware-stream.py"
spec = importlib.util.spec_from_file_location("stream_evidence", FILE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class HardwareEvidenceTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.sender = {
            "probe": "hardware_h264_livekit", "packets_accepted": 300,
            "packets_rejected": 0, "accepted_keyframes": 2,
            "accepted_encoded_bytes": 500000, "sender_observation_seconds": 11,
        }
        self.receiver = {
            "probe": "livekit_decoded_frame_timing", "track": "h264",
            "decoded_frames": 290, "interval_samples": 256,
            "frame_rate_since_first_callback": 28.5,
        }

    def files(self):
        sender, receiver = self.root / "s.log", self.root / "r.log"
        sender.write_text("connecting\n" + json.dumps(self.sender) + "\n", encoding="utf-8")
        receiver.write_text(json.dumps(self.receiver) + "\n", encoding="utf-8")
        return sender, receiver

    def test_observations_cannot_qualify_hardware(self):
        sender, receiver = self.files()
        report_file = self.root / "report.json"
        with redirect_stdout(StringIO()):
            status = mod.main(["--sender", str(sender), "--receiver",
                               str(receiver), "--output", str(report_file)])
        report = json.loads(report_file.read_text(encoding="utf-8"))
        self.assertEqual(status, 0)
        self.assertTrue(report["transport_observations_pass"])
        self.assertFalse(report["physical_gpu_verified"])
        self.assertFalse(report["release_qualified"])

    def test_no_decoded_frames_fails(self):
        self.receiver.update(decoded_frames=0, interval_samples=0,
                             frame_rate_since_first_callback=0)
        sender, receiver = self.files()
        with redirect_stdout(StringIO()):
            self.assertEqual(mod.main(["--sender", str(sender),
                                       "--receiver", str(receiver)]), 2)

    def test_sender_sdk_rejection_fails(self):
        self.sender["packets_rejected"] = 1
        sender, receiver = self.files()
        with redirect_stdout(StringIO()):
            self.assertEqual(mod.main(["--sender", str(sender),
                                       "--receiver", str(receiver)]), 2)

    def test_duplicate_diagnostic_rejected(self):
        sender, _ = self.files()
        sender.write_text(json.dumps(self.sender) + "\n" +
                          json.dumps(self.sender) + "\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            mod.read_probe(sender, mod.SENDER)

    def test_reject_nonfinite_counter(self):
        file = self.root / "gpu.csv"
        file.write_text("TimestampUtc,GpuEnginePath,UtilizationPercent\n"
                        "2026-10-13T12:00:00Z,pid_1_engtype_VideoEncode,nan\n",
                        encoding="utf-8")
        with self.assertRaises(ValueError):
            mod.read_gpu_csv(file)

    def test_gpu_activity_does_not_attribute_encoder(self):
        file = self.root / "gpu.csv"
        file.write_text("TimestampUtc,GpuEnginePath,UtilizationPercent\n" +
                        "".join(f"t{i},pid_1_engtype_VideoEncode,10\n"
                                for i in range(12)), encoding="utf-8")
        gpu = mod.read_gpu_csv(file)
        self.assertTrue(gpu["activity_observed"])
        self.assertFalse(mod.evaluate(self.sender, self.receiver,
                                      gpu)["gpu_process_attribution_verified"])

    def test_boolean_counts_invalid(self):
        self.sender["packets_accepted"] = True
        sender, _ = self.files()
        with self.assertRaises(ValueError):
            mod.read_probe(sender, mod.SENDER)


if __name__ == "__main__":
    unittest.main()
