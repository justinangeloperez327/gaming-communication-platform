"""Local-PC configuration, isolation and command-policy regressions."""
import json
from pathlib import Path
import tempfile
import unittest

from pc_sample import Sample, initialize_sample, sample_lock
from server_stack import initialize


class SampleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config = Path(self.tmp.name) / 'sample'

    def test_separate_ports_loopback_advertisement_and_private_database(self):
        initialize_sample(self.config)
        api = (self.config / 'api.env').read_text()
        self.assertIn('LIVEKIT_URL=ws://127.0.0.1:17880', api)
        self.assertIn('LIVEKIT_CONTROL_URL=http://127.0.0.1:17880', api)
        self.assertIn('@127.0.0.1:15432/gcp', api)
        livekit = (self.config / 'livekit.yaml').read_text()
        self.assertIn('node_ip: 127.0.0.1', livekit)
        self.assertIn('enable_loopback_candidate: true', livekit)
        stack = Sample(self.config)
        self.assertEqual(stack.values['GCP_ENABLE_TLS'], '0')
        self.assertEqual(stack.command.count('-f'), 2)
        self.assertNotIn('--profile', stack.command)
        self.assertNotIn(stack.values['GCP_POSTGRES_ADMIN_PASSWORD'], stack.command)

    def test_disposable_acceptance_uses_a_separate_project(self):
        initialize_sample(self.config, project="gcp-pc-sample-0123456789abcdef")
        self.assertEqual(Sample(self.config).values["GCP_STACK_PROJECT"],
                         "gcp-pc-sample-0123456789abcdef")
        other = Path(self.tmp.name) / "invalid"
        with self.assertRaises(ValueError):
            initialize_sample(other, project="existing-user-stack")
        self.assertFalse(other.exists())

    def test_existing_credentials_cannot_be_overwritten(self):
        initialize_sample(self.config)
        before = (self.config / 'api.env').read_bytes()
        with self.assertRaises(FileExistsError):
            initialize_sample(self.config)
        self.assertEqual(before, (self.config / 'api.env').read_bytes())

    def test_regular_stack_is_not_accepted(self):
        initialize(self.config)
        with self.assertRaises(OSError):
            Sample(self.config)
        (self.config / 'pc-sample.json').write_text(json.dumps({'version': 2}))
        with self.assertRaises(ValueError):
            Sample(self.config)

    def test_concurrent_operations_fail_and_lock_is_reusable(self):
        initialize_sample(self.config)
        with sample_lock(self.config):
            with self.assertRaises(ValueError):
                with sample_lock(self.config):
                    self.fail('Concurrent lock accepted')
        with sample_lock(self.config):
            pass


if __name__ == '__main__':
    unittest.main()
