"""Disposable real-container acceptance of the local-PC network layout.

Runs on a dedicated Linux CI host. This does not qualify Docker Desktop's
Windows media forwarding or physical voice/screen capture.
"""
import json
from pathlib import Path
import secrets
import tempfile

from pc_sample import Sample, initialize_sample
from verify_server_stack import request, wait_status


def main():
    with tempfile.TemporaryDirectory() as folder:
        config = Path(folder) / 'sample'
        initialize_sample(config)
        stack = Sample(config)
        if stack.run('ps', '--all', '-q', capture_output=True, text=True).stdout.strip():
            raise RuntimeError('Refusing to use an existing PC sample project')
        resolved = json.loads(stack.run('config', '--format', 'json', capture_output=True,
                                        text=True).stdout)
        ports = resolved['services']['postgres']['ports']
        assert all(p['host_ip'] == '127.0.0.1' for p in ports)
        assert all(str(p['target']) != '15432' for p in ports)
        try:
            stack.run('build', 'api')
            stack.run('up', '-d', '--wait', '--wait-timeout', '180')
            base = 'http://127.0.0.1:18080'
            request(base + '/health')
            wait_status(base + '/ready', 204)
            account = dict(email='pc-owner@example.test', password=secrets.token_hex(16),
                           handle='pc_owner', display_name='PC sample owner')
            stack.bootstrap_owner(account)
            credentials = {key: account[key] for key in ('email', 'password')}
            login = request(base + '/v1/auth/login', data=credentials)
            assert login['server_owner'] is True
            stack.run('down', '--timeout', '30')
            stack.run('up', '-d', '--wait', '--wait-timeout', '180')
            wait_status(base + '/ready', 204)
            assert request(base + '/v1/auth/login', data=credentials)['server_owner'] is True
            print('Local-PC compose: loopback ports, real migrations/readiness, owner login and persistence passed. No physical media or Windows Docker Desktop qualification.')
        finally:
            stack.run('down', '--volumes', '--timeout', '30')


if __name__ == '__main__':
    main()
