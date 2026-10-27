"""Run a localhost-only GCP sample using Docker Desktop Linux containers."""
import argparse
from contextlib import contextmanager
import getpass
import json
import os
from pathlib import Path
import subprocess
import warnings

from server_stack import ROOT, Stack, initialize, operation_lock, private_file

DEFAULT_CONFIG = ROOT / "deploy/self-hosted/.pc-local"
OVERRIDE = ROOT / "deploy/self-hosted/compose.pc.yaml"


def initialize_sample(config):
    # Fixed separate project and ports: never reuse the regular host stack.
    initialize(config, project="gcp-pc-sample", api_port=18080, db_port=15432,
               signal_port=17880, media_tcp_port=17881, udp_start=17900, udp_end=17920)
    with (config / "stack.env").open("a", encoding="utf-8") as output:
        output.write("GCP_SIGNAL_PORT=17880\nGCP_MEDIA_TCP_PORT=17881\n"
                     "GCP_UDP_START=17900\nGCP_UDP_END=17920\n")
    # The bridge port mapping targets the shared namespace's interface. Only
    # LiveKit signaling binds there; database and control URLs remain loopback.
    livekit = config / "livekit.yaml"
    livekit.write_text(livekit.read_text().replace(
        'bind_addresses: ["127.0.0.1"]', 'bind_addresses: ["0.0.0.0"]'), encoding="utf-8")
    private_file(config / "pc-sample.json", json.dumps({"version": 1}))


class Sample(Stack):
    def __init__(self, config):
        super().__init__(config)
        if (json.loads((self.config / "pc-sample.json").read_text()) != {"version": 1}
                or self.values["GCP_ENABLE_TLS"] != "0"
                or self.values["GCP_STACK_PROJECT"] != "gcp-pc-sample"):
            raise ValueError("Expected a local PC sample configuration")
        self.command += ["-f", str(OVERRIDE)]


@contextmanager
def sample_lock(config):
    if os.name != "nt":
        with operation_lock(config):
            yield
        return
    import msvcrt
    # A byte-range lock is released by the OS even if Python terminates.
    with (config / "operation.lock").open("a+b") as lock:
        if lock.seek(0, 2) == 0:
            lock.write(b"\0")
            lock.flush()
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise ValueError("Another operation is using this sample") from None
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    sub = parser.add_subparsers(dest="action", required=True)
    for name in ("init", "up", "stop", "status"):
        sub.add_parser(name)
    owner = sub.add_parser("owner")
    owner.add_argument("--email", required=True)
    owner.add_argument("--handle", required=True)
    owner.add_argument("--display-name", required=True)
    sub.add_parser("backup").add_argument("archive", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "init":
            initialize_sample(args.config)
            print("Local sample generated. No services started; credentials stay in the config folder.")
            return
        stack = Sample(args.config)
        with sample_lock(stack.config):
            if args.action == "up":
                stack.run("build", "api")
                stack.run("up", "-d", "--wait", "--wait-timeout", "180")
                print("Open GCP and use http://127.0.0.1:18080. Physical media is unverified.")
            elif args.action == "stop":
                stack.run("down", "--timeout", "30")
                print("Sample stopped. Database volume and preferences retained.")
            elif args.action == "status":
                stack.run("ps", "--all")
            elif args.action == "backup":
                stack.backup(args.archive)
                print("Database backup saved. Keep configuration separately.")
            elif args.action == "owner":
                with warnings.catch_warnings():
                    warnings.simplefilter("error", getpass.GetPassWarning)
                    password = getpass.getpass("Owner password: ")
                    confirmation = getpass.getpass("Confirm password: ")
                if password != confirmation or not 12 <= len(password.encode("utf-8")) <= 128:
                    raise ValueError("Passwords must match and contain 12-128 UTF-8 bytes")
                stack.bootstrap_owner(dict(email=args.email, handle=args.handle,
                                           display_name=args.display_name, password=password))
                print("Owner configured. Sign in to the desktop to create invitations.")
    except (ValueError, OSError, subprocess.SubprocessError, getpass.GetPassWarning) as error:
        parser.exit(1, f"PC sample operation failed: {error}\n")


if __name__ == "__main__":
    main()
