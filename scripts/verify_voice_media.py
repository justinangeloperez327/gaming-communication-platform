"""Windows x64 loopback SFU + pinned C++ SDK contracts, synthetic PCM/pixels.

No backend/user secrets, microphone, speakers, physical peers or public service.
The Rust/stack suites separately exercise token authorization and reconciliation.
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import platform
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request
import uuid
import zipfile


VERSION = "1.13.10"
SHA256 = "fd4b7c2c3c1e4c71adaa4f6a046ad1c0abaaaf1e09fae904b35bf231ffae11d2"
IDENTITY = "gcp-session-00000000-0000-0000-0000-000000000041"
ORIGIN = "http://127.0.0.1:17880"
KEY = "gcp-media-ci-key"
LOG_LIMIT = 1024 * 1024
URL = (f"https://github.com/livekit/livekit/releases/download/v{VERSION}/"
       f"livekit_{VERSION}_windows_amd64.zip")
SCREEN_IDENTITY = "gcp-screen-publish-00000000-0000-0000-0000-000000000071"
PROTOCOL_PREFIXES = ("GCP_MEDIA_STAGE:", "GCP_MEDIA_CONTRACT:", "GCP_MEDIA_READY_FOR_EVICTION",
                     "GCP_SCREEN_STAGE:", "GCP_SCREEN_CONTRACT:", "GCP_SCREEN_READY_FOR_EVICTION", "GCP_SCREEN_DESKTOP_CONNECTED")


def token(secret, identity, room, *, publish=False, admin=False,
          sources=None, subscribe=True, ttl=300):
    def encode(value):
        return base64.urlsafe_b64encode(value).rstrip(b"=")
    now = int(time.time())
    grant = {"roomAdmin": True, "room": room} if admin else {
        "roomJoin": True, "room": room, "canPublish": publish,
        "canPublishSources": sources if sources is not None else (["microphone"] if publish else []),
        "canSubscribe": subscribe, "canPublishData": False, "canUpdateOwnMetadata": False,
    }
    claims = {"iss": KEY, "sub": identity, "iat": now, "nbf": now - 2,
              "exp": now + ttl, "jti": secrets.token_hex(16), "video": grant}
    unsigned = b".".join(encode(json.dumps(value, separators=(",", ":")).encode())
                         for value in ({"alg": "HS256", "typ": "JWT"}, claims))
    signature = hmac.new(secret.encode(), unsigned, hashlib.sha256).digest()
    return (unsigned + b"." + encode(signature)).decode()


def clean_environment():
    names = {"DATABASE_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET", "LIVEKIT_URL",
             "LIVEKIT_TOKEN", "GCP_ACCESS_TOKEN", "GCP_REFRESH_TOKEN"}
    return {key: value for key, value in os.environ.items()
            if key.upper() not in names and not key.upper().startswith("GCP_MEDIA_")}


def download_server(root):
    archive = root / "livekit.zip"
    digest = hashlib.sha256()
    total = 0
    with urllib.request.urlopen(URL, timeout=60) as response, archive.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            total += len(chunk)
            if total > 150 * 1024 * 1024:
                raise RuntimeError("SFU archive exceeds size limit")
            digest.update(chunk)
            output.write(chunk)
    if digest.hexdigest() != SHA256:
        raise RuntimeError("SFU archive checksum mismatch")
    return extract_server(archive, root)


def extract_server(archive, root):
    # Extract one exact regular executable; no archive paths are trusted.
    with zipfile.ZipFile(archive) as bundle:
        entries = [item for item in bundle.infolist() if item.filename == "livekit-server.exe"]
        if len(entries) != 1:
            raise RuntimeError("SFU executable member missing or duplicated")
        member = entries[0]
        kind = (member.external_attr >> 16) & 0o170000
        if member.is_dir() or kind not in (0, 0o100000) or member.file_size > 200 * 1024 * 1024:
            raise RuntimeError("Invalid SFU executable member")
        executable = root / "livekit-server.exe"
        with bundle.open(member) as source, executable.open("wb") as output:
            total = 0
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > 200 * 1024 * 1024:
                    raise RuntimeError("SFU executable exceeds size limit")
                output.write(chunk)
        return executable


def control(secret, room, action, identity=IDENTITY):
    # Literal loopback, fixed methods, no redirects or inherited proxy settings.
    if action not in ("GetParticipant", "RemoveParticipant"):
        raise ValueError("Unsupported fixture control operation")
    body = {"room": room, "identity": identity}
    if action == "RemoveParticipant":
        body["revoke_token_ts"] = int(time.time()) + 1
    request = urllib.request.Request(
        f"{ORIGIN}/twirp/livekit.RoomService/{action}", method="POST",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer " +
                 token(secret, "gcp-media-ci-control", room, admin=True)})

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    client = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    with client.open(request, timeout=5) as response:
        body = response.read(65537)
        if len(body) > 65536:
            raise RuntimeError("SFU control response exceeds limit")
        return json.loads(body)


def protocol_lines(log_path):
    if log_path.stat().st_size > LOG_LIMIT:
        raise RuntimeError("Media fixture output exceeds limit")
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    result = [line for line in lines if line.startswith(PROTOCOL_PREFIXES)]
    if any(len(line) > 1024 for line in result):
        raise RuntimeError("Media fixture protocol line exceeds limit")
    return result


def stop(child):
    if child is None or child.poll() is not None:
        return
    child.terminate()
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=5)


def screen_environment(env, secret, room):
    """Disposable fixture grants matching the Rust grant tests; no production API keys."""
    result = dict(env)
    result["GCP_MEDIA_SCREEN_ROOM"] = room
    for variable, identity, publish in [
            ("GCP_MEDIA_SCREEN_PUBLISHER_TOKEN", SCREEN_IDENTITY, True),
            ("GCP_MEDIA_SCREEN_OTHER_TOKEN", "gcp-screen-publish-00000000-0000-0000-0000-000000000072", True),
            ("GCP_MEDIA_SCREEN_VIEWER_TOKEN", "gcp-screen-view-00000000-0000-0000-0000-000000000073", False),
            ("GCP_MEDIA_SCREEN_DESKTOP_TOKEN", "gcp-screen-view-00000000-0000-0000-0000-000000000075", False)]:
        result[variable] = token(secret, identity, room, publish=publish,
                                 sources=["screen_share"] if publish else [],
                                 subscribe=not publish, ttl=60)
    return result


def run_screen(executable, env, secret, root, server, server_log_path):
    executable = executable.resolve(strict=True)
    if not executable.is_file():
        raise RuntimeError("Screen contract executable missing")
    screen_room = "gcp-screen-" + str(uuid.uuid4())
    child = None
    log_path = root / "screen.log"
    with log_path.open("wb") as log:
        try:
            child = subprocess.Popen([str(executable)], cwd=executable.parent,
                                     env=screen_environment(env, secret, screen_room),
                                     stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 120
            evicted = False
            subscribed = False
            last_ping = 0.0
            while child.poll() is None:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Screen contract timed out")
                if server.poll() is not None or server_log_path.stat().st_size > LOG_LIMIT:
                    raise RuntimeError("Loopback SFU exited or exceeded output limit")
                lines = protocol_lines(log_path)
                if time.monotonic() - last_ping >= 1:
                    try:
                        child.stdin.write(b"ping\n")
                        child.stdin.flush()
                    except BrokenPipeError:
                        pass
                    last_ping = time.monotonic()
                if "GCP_SCREEN_DESKTOP_CONNECTED" in lines and not subscribed:
                    participant = control(secret, screen_room, "GetParticipant", SCREEN_IDENTITY)
                    tracks = participant.get("tracks", [])
                    if len(tracks) != 1 or tracks[0].get("source") not in (3, "SCREEN_SHARE") or tracks[0].get("type") not in (1, "VIDEO"):
                        raise RuntimeError("Missing publisher-owned screen for product viewer")
                    sid = tracks[0].get("sid", "")
                    if not sid.startswith("TR_") or len(sid) > 128 or not all(c.isascii() and (c.isalnum() or c in "_-") for c in sid):
                        raise RuntimeError("Invalid real SFU track SID")
                    body = {"room": screen_room, "identity": "gcp-screen-view-00000000-0000-0000-0000-000000000075", "track_sids": [sid], "subscribe": True}
                    request = urllib.request.Request(ORIGIN + "/twirp/livekit.RoomService/UpdateSubscriptions", data=json.dumps(body).encode(),
                        headers={"Content-Type": "application/json", "Authorization": "Bearer " + token(secret, "fixture-control", screen_room, admin=True)})
                    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as response:
                        if response.status != 200:
                            raise RuntimeError("Product viewer subscription rejected")
                    child.stdin.write(f"track {sid}\nnext\n".encode("ascii")); child.stdin.flush()
                    subscribed = True
                if "GCP_SCREEN_READY_FOR_EVICTION" in lines and not evicted:
                    participant = control(secret, screen_room, "GetParticipant", SCREEN_IDENTITY)
                    tracks = participant.get("tracks", [])
                    if (participant.get("identity") != SCREEN_IDENTITY or len(tracks) != 1 or
                            tracks[0].get("source") not in (3, "SCREEN_SHARE") or
                            tracks[0].get("type") not in (1, "VIDEO")):
                        raise RuntimeError("Expected exact publisher-owned screen video track")
                    control(secret, screen_room, "RemoveParticipant", SCREEN_IDENTITY)
                    evicted = True
                time.sleep(0.1)
            lines = protocol_lines(log_path)
            for line in lines:
                print(line)
            if child.returncode != 0 or not evicted or not subscribed or not any(
                    line.startswith("GCP_SCREEN_CONTRACT:passed ") for line in lines):
                raise RuntimeError("Screen authorization contract did not pass")
        finally:
            stop(child)


def run(executable, screen_executable=None):
    if platform.system() != "Windows" or platform.machine() not in ("AMD64", "x86_64"):
        raise RuntimeError("The pinned media fixture supports Windows x64 only")
    executable = executable.resolve(strict=True)
    if not executable.is_file():
        raise RuntimeError("Media contract executable missing")
    for port, kind in [(17880, socket.SOCK_STREAM), (17881, socket.SOCK_STREAM),
                       *((port, socket.SOCK_DGRAM) for port in range(17882, 17914))]:
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(("127.0.0.1", port))
    with tempfile.TemporaryDirectory(prefix="gcp-voice-media-") as directory:
        root = Path(directory)
        server_executable = download_server(root)
        secret = secrets.token_hex(32)
        room = "gcp-voice-" + str(uuid.uuid4())
        config = root / "livekit.yaml"
        config.write_text(f"""port: 17880
bind_addresses: ["127.0.0.1"]
rtc:
  node_ip: 127.0.0.1
  use_external_ip: false
  tcp_port: 17881
  port_range_start: 17882
  port_range_end: 17913
keys:
  {KEY}: {secret}
logging:
  level: error
""", encoding="utf-8")
        env = clean_environment()
        server = media = None
        log_path = root / "media.log"
        server_log_path = root / "server.log"
        with server_log_path.open("wb") as server_log, log_path.open("wb") as media_log:
            try:
                server = subprocess.Popen([str(server_executable), "--config", str(config)],
                                          env=env, stdout=server_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("Loopback SFU exited during startup")
                    try:
                        with socket.create_connection(("127.0.0.1", 17880), timeout=1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Loopback SFU startup timed out") from None
                        time.sleep(0.1)
                env.update({
                    "GCP_MEDIA_PUBLISHER_TOKEN": token(secret, IDENTITY, room, publish=True),
                    "GCP_MEDIA_SUBSCRIBER_TOKEN": token(secret, "gcp-media-ci-receiver", room),
                    "GCP_MEDIA_ISOLATED_TOKEN": token(secret, "gcp-media-ci-isolated", room + "-isolated"),
                })
                media = subprocess.Popen([str(executable)], cwd=executable.parent, env=env,
                                         stdout=media_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 120
                evicted = False
                while media.poll() is None:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Media contract timed out")
                    if server.poll() is not None or server_log_path.stat().st_size > LOG_LIMIT:
                        raise RuntimeError("Loopback SFU exited or exceeded output limit")
                    lines = protocol_lines(log_path)
                    if "GCP_MEDIA_READY_FOR_EVICTION" in lines and not evicted:
                        participant = control(secret, room, "GetParticipant")
                        if participant.get("identity") != IDENTITY or not participant.get("tracks"):
                            raise RuntimeError("Expected an active publisher before eviction")
                        control(secret, room, "RemoveParticipant")
                        evicted = True
                    time.sleep(0.1)
                lines = protocol_lines(log_path)
                for line in lines:
                    print(line)
                if media.returncode != 0 or not evicted or not any(
                        line.startswith("GCP_MEDIA_CONTRACT:passed ") for line in lines):
                    raise RuntimeError("Real media assertions did not pass")
                if screen_executable is not None:
                    run_screen(screen_executable, env, secret, root, server, server_log_path)
            finally:
                stop(media)
                stop(server)
        print(f"LiveKit {VERSION} synthetic PCM transport/mute/isolation/rejoin/eviction passed; physical audio unverified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--screen-executable", type=Path)
    args = parser.parse_args()
    run(args.executable, args.screen_executable)
