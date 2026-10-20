"""Run the ignored Rust contract against a disposable, checksum-pinned SFU.

Linux x64 CI/development only. No media peers or physical acceptance evidence.
"""

import hashlib
from pathlib import Path
import platform
import socket
import subprocess
import tarfile
import tempfile
import time
import urllib.request


VERSION = "1.13.10"
SHA256 = "c25a3e45fb922e9a93c6443d10c0ed041ba64342097e38632331736630283165"
TEST = "livekit_eviction::control_contract::real_roomservice_contract"


def main():
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        raise RuntimeError("The pinned fixture supports Linux x64 only")
    # Refuse to run against an unrelated service already occupying our port.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 17880))
    with tempfile.TemporaryDirectory(prefix="gcp-livekit-contract-") as directory:
        root = Path(directory)
        archive = root / "livekit.tar.gz"
        url = (f"https://github.com/livekit/livekit/releases/download/v{VERSION}/"
               f"livekit_{VERSION}_linux_amd64.tar.gz")
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            total = 0
            while chunk := response.read(1024 * 1024):
                total += len(chunk)
                if total > 150 * 1024 * 1024:
                    raise RuntimeError("LiveKit fixture archive exceeds size limit")
                output.write(chunk)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError("LiveKit fixture checksum mismatch")
        # Extract only the regular executable, never arbitrary archive paths.
        with tarfile.open(archive) as bundle:
            member = bundle.getmember("livekit-server")
            if not member.isfile() or member.size > 200 * 1024 * 1024:
                raise RuntimeError("Invalid LiveKit executable member")
            source = bundle.extractfile(member)
            if source is None:
                raise RuntimeError("Missing LiveKit executable")
            with source:
                (root / "livekit-server").write_bytes(source.read())
        executable = root / "livekit-server"
        executable.chmod(0o700)
        config = root / "livekit.yaml"
        config.write_text("""port: 17880
bind_addresses: ["127.0.0.1"]
rtc:
  node_ip: 127.0.0.1
  use_external_ip: false
  tcp_port: 17881
  port_range_start: 17882
  port_range_end: 17892
keys:
  gcp-contract-key: disposable-loopback-contract-secret-32bytes
logging:
  level: warn
""")
        with (root / "server.log").open("w+") as log:
            server = subprocess.Popen([str(executable), "--config", str(config)],
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 20
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("LiveKit exited before becoming ready")
                    try:
                        with socket.create_connection(("127.0.0.1", 17880), timeout=1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("LiveKit startup timed out") from None
                        time.sleep(0.2)
                result = subprocess.run(
                    ["cargo", "test", "-p", "gaming-comms-server", "--lib", "--locked",
                     TEST, "--", "--ignored", "--exact"],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
                print(result.stdout)
                result.check_returncode()
                if "test result: ok. 1 passed; 0 failed; 0 ignored;" not in result.stdout:
                    raise RuntimeError("The selected contract test did not run exactly once")
                if server.poll() is not None:
                    raise RuntimeError("LiveKit exited during the contract test")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(64 * 1024))
                raise
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)
        print(f"LiveKit {VERSION} control contract passed; media remains unverified")


if __name__ == "__main__":
    main()
