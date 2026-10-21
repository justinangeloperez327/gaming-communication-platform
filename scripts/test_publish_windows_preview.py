import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import publish_windows_preview as release


class PreviewReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(release.os.environ, {
            "GITHUB_REPOSITORY": "justinangeloperez327/gaming-communication-platform",
            "GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123",
        })
        self.env.start()
        self.addCleanup(self.env.stop)
        (self.root / "portable").mkdir()
        (self.root / "installer").mkdir()
        self.zip = self.root / "portable/gcp-private-windows-x64.zip"
        self.installer = self.root / "installer/GCP-0.1.0-alpha.1-windows-x64-setup.exe"
        for file in [self.zip, self.installer]:
            file.write_bytes(b"fixture binary payload")
        digest = hashlib.sha256(b"fixture binary payload").hexdigest()
        (self.root / "portable/gcp-private-windows-x64.sha256").write_text(digest)
        self.installer.with_suffix(".exe.sha256").write_text(digest)

    @staticmethod
    def missing():
        return subprocess.CalledProcessError(1, ["gh"], stderr="HTTP 404: Not Found")

    def test_create_draft_upload_then_publish_exact_commit(self):
        with patch.object(release, "gh", side_effect=[self.missing(), "", "", ""]) as gh:
            release.publish(self.root)
        calls = [call.args for call in gh.call_args_list]
        self.assertEqual([args[:2] for args in calls[1:]],
                         [("release", "create"), ("release", "upload"), ("release", "edit")])
        self.assertIn("--draft", calls[1])
        self.assertIn("a" * 40, calls[1])
        self.assertIn("--prerelease", calls[1])
        provenance = json.loads((self.root / "publish/BUILD.json").read_text())
        self.assertEqual(provenance["commit"], "a" * 40)
        self.assertEqual(len((self.root / "publish/SHA256SUMS.txt").read_text().splitlines()), 2)

    def test_published_release_is_never_replaced(self):
        with patch.object(release, "gh", return_value=json.dumps({"draft": False})) as gh:
            release.publish(self.root)
        self.assertEqual(gh.call_count, 1)
        self.assertFalse((self.root / "publish").exists())

    def test_other_commit_draft_is_not_overwritten(self):
        with patch.object(release, "gh", return_value=json.dumps({"draft": True, "target_commitish": "b" * 40})) as gh:
            with self.assertRaisesRegex(ValueError, "different commit"):
                release.publish(self.root)
        self.assertEqual(gh.call_count, 1)

    def test_same_commit_draft_can_resume_upload(self):
        existing = json.dumps({"draft": True, "target_commitish": "a" * 40})
        with patch.object(release, "gh", side_effect=[existing, "", ""]) as gh:
            release.publish(self.root)
        self.assertEqual(gh.call_args_list[1].args[:2], ("release", "upload"))
        self.assertEqual(gh.call_args_list[2].args[:2], ("release", "edit"))

    def test_corrupted_installer_blocks_creation_and_publication(self):
        self.installer.write_bytes(b"tampered")
        with patch.object(release, "gh", side_effect=self.missing()) as gh:
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                release.publish(self.root)
        self.assertEqual(gh.call_count, 1)

    def test_api_failure_is_not_misread_as_missing_release(self):
        error = subprocess.CalledProcessError(1, ["gh"], stderr="HTTP 403: Forbidden")
        with patch.object(release, "gh", side_effect=error) as gh:
            with self.assertRaises(subprocess.CalledProcessError):
                release.publish(self.root)
        self.assertEqual(gh.call_count, 1)


if __name__ == "__main__":
    unittest.main()
