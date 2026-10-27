"""Offline guards for the loopback voice media runner; not media evidence."""
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import verify_voice_media as media


class MediaRunnerGuards(unittest.TestCase):
    def test_participant_grants_and_signature_are_separate_from_admin(self):
        secret = "disposable-loopback-secret"
        for publish in (False, True):
            jwt = media.token(secret, "fixture-peer", "private-fixture-room", publish=publish)
            head, payload, signature = jwt.split(".")
            claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
            self.assertEqual(claims["video"]["room"], "private-fixture-room")
            self.assertEqual(claims["video"]["canPublish"], publish)
            self.assertNotIn("roomAdmin", claims["video"])
            self.assertFalse(claims["video"]["canPublishData"])
            self.assertLessEqual(claims["exp"] - claims["iat"], 300)
            self.assertEqual(claims["video"]["canPublishSources"], ["microphone"] if publish else [])
            self.assertTrue(hmac.compare_digest(
                hmac.new(secret.encode(), (head + "." + payload).encode(), hashlib.sha256).digest(),
                base64.urlsafe_b64decode(signature + "=" * (-len(signature) % 4))))

    def test_child_environment_filters_credentials_case_insensitively(self):
        with patch.dict(os.environ, {"LiVeKiT_ApI_SeCrEt": "private", "GCP_MEDIA_PUBLISHER_TOKEN": "stale",
                                     "LIVEKIT_TOKEN": "stale", "PATH": "runtime"}, clear=True):
            self.assertEqual(media.clean_environment(), {"PATH": "runtime"})

    def test_screen_fixture_grants_separate_publisher_and_viewer(self):
        env = media.screen_environment({"PATH": "runtime"}, "disposable-screen-secret", "gcp-screen-fixture")
        self.assertEqual(env["PATH"], "runtime")
        for name in ("PUBLISHER", "OTHER", "VIEWER"):
            jwt = env[f"GCP_MEDIA_SCREEN_{name}_TOKEN"]
            head, body, signature = jwt.split(".")
            claims = json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
            publish = name != "VIEWER"
            self.assertEqual(claims["video"]["room"], "gcp-screen-fixture")
            self.assertEqual(claims["video"]["canPublish"], publish)
            self.assertEqual(claims["video"]["canPublishSources"], ["screen_share"] if publish else [])
            self.assertEqual(claims["video"]["canSubscribe"], not publish)
            self.assertFalse(claims["video"]["canPublishData"])
            self.assertNotIn("roomAdmin", claims["video"])
            self.assertEqual(claims["exp"] - claims["iat"], 60)
            self.assertTrue(hmac.compare_digest(
                hmac.new(b"disposable-screen-secret", (head + "." + body).encode(), hashlib.sha256).digest(),
                base64.urlsafe_b64decode(signature + "=" * (-len(signature) % 4))))

    def test_extraction_ignores_traversal_and_only_extracts_exact_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with zipfile.ZipFile(root / "fixture.zip", "w") as bundle:
                bundle.writestr("../escape.txt", "untrusted")
                bundle.writestr("livekit-server.exe", b"fixture-not-executable")
            result = media.extract_server(root / "fixture.zip", root)
            self.assertEqual(result.read_bytes(), b"fixture-not-executable")
            self.assertEqual(sorted(item.name for item in root.iterdir()), ["fixture.zip", "livekit-server.exe"])

    def test_extraction_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            member = zipfile.ZipInfo("livekit-server.exe")
            member.create_system = 3
            member.external_attr = 0o120777 << 16
            with zipfile.ZipFile(root / "fixture.zip", "w") as bundle:
                bundle.writestr(member, "elsewhere")
            with self.assertRaises(RuntimeError):
                media.extract_server(root / "fixture.zip", root)
            self.assertFalse((root / "livekit-server.exe").exists())

    def test_missing_executable_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with zipfile.ZipFile(root / "fixture.zip", "w") as bundle:
                bundle.writestr("nested/livekit-server.exe", "wrong path")
            with self.assertRaises(RuntimeError):
                media.extract_server(root / "fixture.zip", root)

    def test_output_is_bounded_and_only_contract_lines_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "media.log"
            log.write_text("SDK diagnostics must not be echoed\nGCP_MEDIA_STAGE:fixture\n", encoding="utf-8")
            self.assertEqual(media.protocol_lines(log), ["GCP_MEDIA_STAGE:fixture"])
            log.write_text("GCP_MEDIA_STAGE:" + "x" * 1024, encoding="utf-8")
            with self.assertRaises(RuntimeError):
                media.protocol_lines(log)
            log.write_bytes(b"x" * (media.LOG_LIMIT + 1))
            with self.assertRaises(RuntimeError):
                media.protocol_lines(log)


if __name__ == "__main__":
    unittest.main()
