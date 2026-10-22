"""Real-container acceptance for a disposable GCP stack (Linux Docker Engine).

Creates random project names and deletes ONLY those projects' volumes. Never
accepts an operator configuration. No microphone, media peers or deployed host.
"""

import base64
import hashlib
import hmac
import http.client
import json
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from server_stack import Stack, initialize


def request(url, *, data=None, token=None, expected=200, ssl_context=None, source_ip=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    body = None if data is None else json.dumps(data).encode()
    query = urllib.request.Request(url, data=body, headers=headers)
    # Loopback acceptance must not inherit a runner's HTTP proxy.
    handlers = [urllib.request.ProxyHandler({})]
    if ssl_context:
        class Connection(http.client.HTTPSConnection):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, source_address=(source_ip or "127.0.0.1", 0), **kwargs)

        class Handler(urllib.request.HTTPSHandler):
            def https_open(self, query):
                return self.do_open(Connection, query, context=self._context)

        handlers.append(Handler(context=ssl_context))
    opener = urllib.request.build_opener(*handlers)
    try:
        response = opener.open(query, timeout=12)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        if response.status != expected:
            raise AssertionError(f"Unexpected HTTP status {response.status}; expected {expected}")
        payload = response.read(65537)
        if len(payload) > 65536:
            raise AssertionError("Acceptance response exceeded its size bound")
        if payload and "json" in response.headers.get("Content-Type", ""):
            return json.loads(payload)
        return payload or None


def wait_status(url, expected, seconds=90, **kwargs):
    deadline = time.monotonic() + seconds
    while True:
        try:
            request(url, expected=expected, **kwargs)
            return
        except (AssertionError, OSError):
            if time.monotonic() >= deadline:
                raise AssertionError(f"Endpoint did not reach HTTP {expected}") from None
            time.sleep(0.5)


def credentials(config):
    return dict(line.split("=", 1) for line in (config / "api.env").read_text().splitlines())


def encode(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode()


def verify_grant(grant, values, room):
    assert grant["server_url"] == values["LIVEKIT_URL"]
    assert grant["room"] == room
    parts = grant["participant_token"].split(".")
    signature = encode(hmac.digest(values["LIVEKIT_API_SECRET"].encode(),
                                   ".".join(parts[:2]).encode(), "sha256"))
    assert hmac.compare_digest(signature, parts[2])
    claims = json.loads(base64.urlsafe_b64decode(parts[1] + "=="))
    assert claims["iss"] == values["LIVEKIT_API_KEY"]
    assert claims["video"]["room"] == room
    assert claims["video"]["canPublishSources"] == ["microphone"]


def control_token(values, video):
    now = int(time.time())
    content = ".".join([encode(b'{"alg":"HS256","typ":"JWT"}'),
                        encode(json.dumps({"iss": values["LIVEKIT_API_KEY"], "iat": now,
                                           "nbf": now - 2, "exp": now + 30,
                                           "video": video}).encode())])
    return content + "." + encode(hmac.digest(values["LIVEKIT_API_SECRET"].encode(),
                                             content.encode(), "sha256"))


def exercise(stack, base, email, password, *, create=False):
    session = request(base + "/v1/auth/login", data={"email": email, "password": password})
    token = session["access_token"]
    request(base + "/v1/me", expected=401)
    me = request(base + "/v1/me", token=token)
    assert me["email"] == email
    if create:
        community = request(base + "/v1/communities", data={"name": "Stack acceptance"},
                            token=token, expected=201)
        request(base + f"/v1/communities/{community['id']}/channels", expected=201,
                data={"name": "voice", "channel_kind": "voice", "position": 0}, token=token)
    communities = request(base + "/v1/communities", token=token)
    assert len(communities) == 1
    community = communities[0]["id"]
    channels = request(base + f"/v1/communities/{community}/channels", token=token)
    assert len(channels) == 1
    channel = channels[0]["id"]
    path = f"/v1/communities/{community}/channels/{channel}/livekit-token"
    grant = request(base + path, data={}, token=token, expected=201)
    verify_grant(grant, credentials(stack.config), "gcp-voice-" + channel)
    return token, path, grant["room"]


def main():
    # Fail before creating containers if fixed acceptance ports are occupied.
    for port in (18080, 15432, 17880, 17881, 18443, 18444):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", port))
    stacks = []
    with tempfile.TemporaryDirectory(prefix="gcp-stack-acceptance-") as directory:
        root = Path(directory)
        try:
            for label in ("source", "restore"):
                config = root / label
                initialize(config, project="gcp-accept-" + secrets.token_hex(6),
                           api_port=18080, db_port=15432, signal_port=17880,
                           media_tcp_port=17881, udp_start=17900, udp_end=17920)
                stacks.append(Stack(config))
            source, restored = stacks
            source.run("build", "api")
            source.run("up", "-d", "--wait", "--wait-timeout", "180")
            base = "http://127.0.0.1:18080"
            request(base + "/health")
            wait_status(base + "/ready", 204)
            email, password = "stack@example.test", secrets.token_hex(16)
            request(base + "/v1/auth/register", data={"email": email, "password": password,
                    "handle": "stack_owner", "display_name": "Stack owner"}, expected=201)
            token, grant_path, room = exercise(source, base, email, password, create=True)

            # Authenticate against the real SFU with the same keys as API grants.
            values = credentials(source.config)
            rpc = "http://127.0.0.1:17880/twirp/livekit.RoomService/"
            request(rpc + "CreateRoom", data={"name": room},
                    token=control_token(values, {"roomCreate": True}))
            rooms = request(rpc + "ListRooms", data={},
                            token=control_token(values, {"roomList": True}))
            assert any(item["name"] == room for item in rooms["rooms"])
            request(rpc + "ListRooms", data={}, token="invalid-token", expected=401)

            source.run("stop", "livekit")
            wait_status(base + "/ready", 503, seconds=45)
            request(base + grant_path, data={}, token=token, expected=503)
            request(base + "/health")
            source.run("start", "livekit")
            wait_status(base + "/ready", 204)
            source.run("stop", "postgres")
            wait_status(base + "/ready", 503)
            request(base + "/health")
            source.run("start", "postgres")
            wait_status(base + "/ready", 204)

            # SIGTERM must exit cleanly, rather than being killed by Docker.
            container = source.run("ps", "-a", "-q", "api", capture_output=True,
                                   text=True).stdout.strip()
            source.run("stop", "-t", "15", "api")
            status = subprocess.run(["docker", "inspect", "--format", "{{.State.ExitCode}}",
                                     container], capture_output=True, text=True, check=True,
                                    timeout=15).stdout.strip()
            assert status == "0", "API did not exit cleanly on SIGTERM"

            # Advertised WSS must not require public DNS for private control RPCs.
            api_env = source.config / "api.env"
            api_env.write_text(api_env.read_text().replace("LIVEKIT_URL=ws://127.0.0.1:17880",
                                                          "LIVEKIT_URL=wss://voice.fixture.invalid"))
            source.run("up", "-d", "--wait", "--wait-timeout", "180", "api")
            exercise(source, base, email, password)
            archive = root / "database.pgdump"
            source.backup(archive)
            assert archive.stat().st_size > 100
            source.run("down", "--timeout", "30")  # volumes intentionally retained
            source.run("up", "-d", "--wait", "--wait-timeout", "180")
            exercise(source, base, email, password)  # persisted users AND channel data
            source.run("down", "--timeout", "30")
            try:
                source.restore(archive)
            except ValueError as error:
                assert "not empty" in str(error)
            else:
                raise AssertionError("Restore accepted a populated database")
            source.run("down", "--timeout", "30")

            restored.restore(archive)  # new volume, new DB password and SFU keys
            restored.run("build", "api")
            restored.run("up", "-d", "--wait", "--wait-timeout", "180")
            exercise(restored, base, email, password)

            # Validate the actual public template without requesting certificates.
            remote = root / "remote"
            initialize(remote, project="gcp-config-" + secrets.token_hex(6),
                       api_port=18080, db_port=15432, signal_port=17880, media_tcp_port=17881,
                       api_domain="api.example.test", voice_domain="voice.example.test",
                       media_ip="192.0.2.5", allow_cidrs=["127.0.0.1/32"])
            remote_stack = Stack(remote)
            stacks.append(remote_stack)
            remote_stack.run("run", "--rm", "--no-deps", "caddy", "caddy", "validate",
                             "--config", "/etc/caddy/Caddyfile", "--adapter", "caddyfile")
            # Exercise that proxy policy over real TLS using a disposable local
            # CA. Trust that CA explicitly; never disable certificate validation.
            caddy = remote / "Caddyfile"
            caddy.write_text(caddy.read_text()
                             .replace("admin off", "admin off\n  auto_https disable_redirects")
                             .replace("api.example.test {", "https://localhost:18443 {\n  tls internal")
                             .replace("voice.example.test {", "https://localhost:18444 {\n  tls internal"))
            remote_stack.run("up", "-d", "--no-deps", "caddy")
            deadline = time.monotonic() + 30
            while True:
                try:
                    certificate = remote_stack.run(
                        "exec", "-T", "caddy", "cat", "/data/caddy/pki/authorities/local/root.crt",
                        capture_output=True, text=True).stdout
                    context = ssl.create_default_context(cadata=certificate)
                    break
                except (subprocess.SubprocessError, ssl.SSLError):
                    if time.monotonic() >= deadline:
                        raise AssertionError("Local TLS fixture did not initialize") from None
                    time.sleep(0.5)
            wait_status("https://localhost:18443/ready", 204, ssl_context=context)
            request("https://localhost:18443/ready", expected=403,
                    ssl_context=context, source_ip="127.0.0.2")
            request("https://localhost:18444/twirp/livekit.RoomService/ListRooms", data={},
                    token=control_token(credentials(restored.config), {"roomList": True}),
                    ssl_context=context)
            print("PASS: fresh migrations, real API/SFU credentials, dependency outages, SIGTERM, "
                  "WSS/control separation, restart persistence, empty-target restore, TLS proxy "
                  "and source-IP access denial.")
            print("No media peers, physical audio, external TLS issuance or deployed host verified.")
        except BaseException:
            # Bound and redact diagnostics; never upload generated configuration.
            for stack in stacks:
                logs = stack.run("logs", "--no-color", "--tail", "60", capture_output=True,
                                 text=True).stdout[-20000:]
                for file in ("api.env", "stack.env"):
                    for line in (stack.config / file).read_text().splitlines():
                        key, value = line.split("=", 1)
                        if any(part in key for part in ("PASSWORD", "SECRET", "KEY", "DATABASE_URL")):
                            logs = logs.replace(value, "[redacted]")
                print(logs)
            raise
        finally:
            for stack in reversed(stacks):
                try:
                    stack.run("down", "--volumes", "--remove-orphans", "--timeout", "15",
                              stdout=subprocess.DEVNULL)
                except subprocess.SubprocessError:
                    print("Disposable project cleanup failed; inspect Docker resources on this runner.")


if __name__ == "__main__":
    main()
