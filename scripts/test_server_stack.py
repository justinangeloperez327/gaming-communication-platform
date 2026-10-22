"""Offline checks for credential generation and destructive-operation guards."""

from pathlib import Path
import os
import stat
import tempfile
import unittest
from unittest.mock import Mock

from server_stack import Stack, initialize


class StackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = self.root / "config"

    def test_local_configuration_is_private_unique_and_not_overwritten(self):
        initialize(self.config)
        first = (self.config / "api.env").read_text()
        self.assertIn("LIVEKIT_URL=ws://127.0.0.1:7880", first)
        self.assertIn("GCP_RUN_MIGRATIONS=0", first)
        if os.name == "posix":  # Deployment is Linux-only; Windows has no POSIX mode bits.
            self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((self.config / "api.env").stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE((self.config / "stack.env").stat().st_mode), 0o600)
        with self.assertRaises(FileExistsError):
            initialize(self.config)
        self.assertEqual(first, (self.config / "api.env").read_text())
        other = self.root / "other"
        initialize(other)
        self.assertNotEqual(first, (other / "api.env").read_text())

    def test_remote_configuration_separates_public_and_private_origins(self):
        initialize(self.config, api_domain="api.example.test", voice_domain="voice.example.test",
                   media_ip="192.0.2.5", allow_cidrs=["198.51.100.10/32"])
        api = (self.config / "api.env").read_text()
        self.assertIn("LIVEKIT_URL=wss://voice.example.test", api)
        self.assertIn("LIVEKIT_CONTROL_URL=http://127.0.0.1:7880", api)
        caddy = (self.config / "Caddyfile").read_text()
        self.assertIn("@friends remote_ip 198.51.100.10/32", caddy)
        self.assertIn("respond 403", caddy)
        self.assertIn("node_ip: 192.0.2.5", (self.config / "livekit.yaml").read_text())

    def test_bad_remote_configuration_cannot_write_secrets(self):
        common = dict(api_domain="api.example.test", voice_domain="voice.example.test",
                      media_ip="192.0.2.5", allow_cidrs=["198.51.100.0/24"])
        cases = [dict(api_domain="api.example.test { respond 200 }"), dict(allow_cidrs=[]),
                 dict(allow_cidrs=["0.0.0.0/0"]), dict(allow_cidrs=["::/0"]),
                 dict(voice_domain="api.example.test"), dict(media_ip="127.0.0.1")]
        for overrides in cases:
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                initialize(self.config, **(common | overrides))
            self.assertFalse(self.config.exists())
        for options in [dict(api_port=5432), dict(udp_start=10), dict(udp_end=49999),
                        dict(project="../other"), dict(api_domain="api.example.test")]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                initialize(self.config, **options)
            self.assertFalse(self.config.exists())

    def test_restore_refuses_nonempty_schema_without_running_pg_restore(self):
        initialize(self.config)
        stack = Stack(self.config)
        stack.run = Mock(return_value=Mock(stdout=""))
        stack.database = Mock(return_value=Mock(stdout="42\n"))
        archive = self.root / "backup.pgdump"
        archive.write_bytes(b"PGDMPfixture")
        with self.assertRaisesRegex(ValueError, "not empty"):
            stack.restore(archive)
        self.assertEqual(stack.database.call_count, 1)
        self.assertEqual(stack.database.call_args.args[0], "psql")

    def test_restore_refuses_running_writers_and_invalid_archives(self):
        initialize(self.config)
        stack = Stack(self.config)
        stack.run = Mock(return_value=Mock(stdout="running-api-id"))
        stack.database = Mock()
        archive = self.root / "backup.pgdump"
        archive.write_bytes(b"not-a-backup")
        with self.assertRaisesRegex(ValueError, "Stop the API"):
            stack.restore(archive)
        stack.run.return_value.stdout = ""
        with self.assertRaisesRegex(ValueError, "custom-format"):
            stack.restore(archive)
        stack.database.assert_not_called()

    def test_failed_backup_is_removed_and_existing_backup_is_preserved(self):
        initialize(self.config)
        stack = Stack(self.config)
        stack.database = Mock(side_effect=OSError("fixture failure"))
        archive = self.root / "backup.pgdump"
        with self.assertRaises(OSError):
            stack.backup(archive)
        self.assertFalse(archive.exists())
        archive.write_bytes(b"keep-existing-backup")
        with self.assertRaises(FileExistsError):
            stack.backup(archive)
        self.assertEqual(archive.read_bytes(), b"keep-existing-backup")


if __name__ == "__main__":
    unittest.main()
