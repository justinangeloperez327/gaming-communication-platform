#!/usr/bin/env python3
"""Actual Qt/Rust chat acceptance against an explicitly disposable loopback DB."""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
from urllib.parse import urlparse
from urllib.request import build_opener, ProxyHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    database = urlparse(os.environ.get("DATABASE_URL", ""))
    # Migrations/bootstrap are only permitted in this dedicated CI database.
    if database.hostname not in ("127.0.0.1", "localhost") or database.path != "/gcp_chat_test":
        raise SystemExit("Use the disposable loopback gcp_chat_test database")
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith("LIVEKIT_") or key.startswith("GCP_LIVEKIT_"):
            env.pop(key)
    env["RUST_LOG"] = "gaming_comms_server=info"
    password = secrets.token_hex(24)
    owner = {"handle": "chat_owner", "display_name": "Chat Owner", "email": "chat-owner@example.test", "password": password}
    subprocess.run([str(args.server.resolve()), "--migrate-only"], env=env, check=True, timeout=60)
    subprocess.run([str(args.server.resolve()), "--bootstrap-owner"], input=json.dumps(owner), text=True, env=env, check=True, timeout=30)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    env["GCP_BIND_ADDRESS"] = f"127.0.0.1:{port}"
    origin = f"http://127.0.0.1:{port}"
    opener = build_opener(ProxyHandler({}))
    with tempfile.TemporaryFile(mode="w+") as log:
        server = subprocess.Popen([str(args.server.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 20
            while True:
                if server.poll() is not None:
                    raise RuntimeError("disposable API stopped before readiness")
                try:
                    with opener.open(origin + "/ready", timeout=1) as response:
                        if response.status == 204:
                            break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("disposable API readiness timed out")
                time.sleep(0.1)
            client_env = env | {"GCP_CHAT_ORIGIN": origin, "GCP_CHAT_PASSWORD": password}
            subprocess.run([str(args.client.resolve())], env=client_env, check=True, timeout=150)
        except BaseException:
            log.seek(0)
            print(log.read(16000))
            raise
        finally:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)


if __name__ == "__main__":
    main()
