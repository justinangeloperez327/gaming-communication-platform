"""Manage a single-host Linux GCP stack without shell-evaluating credentials.

No deployment is performed by init. Every Docker command uses a fixed Compose
file and an explicit project/config directory. stop retains all data volumes.
"""

import argparse
from contextlib import contextmanager
import ipaddress
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPOSE = ROOT / "deploy/self-hosted/compose.yaml"
DEFAULT_CONFIG = ROOT / "deploy/self-hosted/.local"


def private_file(path, content):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as output:
        output.write(content)


def domain(raw):
    # These values enter Caddy syntax; accept DNS names, never arbitrary config.
    value = raw.lower()
    if (len(value) > 253 or "." not in value or
            any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label)
                for label in value.split("."))):
        raise ValueError("Use a DNS name without a scheme, port, path or wildcard")
    try:
        ipaddress.ip_address(value)
    except ValueError:
        return value
    raise ValueError("TLS endpoints require DNS names")


def initialize(config, *, project="gcp", api_port=8080, db_port=5432,
               signal_port=7880, media_tcp_port=7881, udp_start=50000,
               udp_end=50100, api_domain=None, voice_domain=None,
               media_ip=None, allow_cidrs=()):
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,39}", project):
        raise ValueError("Project must be 1-40 lowercase letters, digits or hyphens")
    ports = [api_port, db_port, signal_port, media_tcp_port]
    if (any(not 1024 <= p <= 65535 for p in ports + [udp_start, udp_end]) or
            len(set(ports)) != len(ports) or udp_end < udp_start):
        raise ValueError("Use distinct unprivileged TCP ports and an ordered UDP range")
    public = any([api_domain, voice_domain, media_ip, allow_cidrs])
    if public and not all([api_domain, voice_domain, media_ip, allow_cidrs]):
        raise ValueError("Remote mode requires both domains, a media IP and --allow-cidr")
    if public:
        api_domain, voice_domain = domain(api_domain), domain(voice_domain)
        if api_domain == voice_domain:
            raise ValueError("API and voice require different DNS names")
        media_ip = str(ipaddress.IPv4Address(media_ip))
        address = ipaddress.ip_address(media_ip)
        if address.is_loopback or address.is_unspecified or address.is_multicast:
            raise ValueError("Remote media IP must be a reachable unicast IPv4 address")
        networks = [str(ipaddress.ip_network(cidr, strict=False)) for cidr in allow_cidrs]
        if any(ipaddress.ip_network(cidr).prefixlen == 0 for cidr in networks):
            raise ValueError("Use the intended friends' CIDRs rather than the whole internet")
    else:
        media_ip, networks = "127.0.0.1", []

    # Never overwrite an installation's credentials or silently rotate DB keys.
    config.mkdir(mode=0o700, parents=True, exist_ok=False)
    config.chmod(0o700)
    db_password, admin_password = secrets.token_hex(32), secrets.token_hex(32)
    key, secret = "gcp-" + secrets.token_hex(12), secrets.token_hex(32)
    voice_url = f"wss://{voice_domain}" if public else f"ws://127.0.0.1:{signal_port}"
    private_file(config / "stack.env", (
        f"GCP_STACK_PROJECT={project}\nGCP_DB_PORT={db_port}\n"
        f"GCP_POSTGRES_ADMIN_PASSWORD={admin_password}\n"
        f"GCP_ENABLE_TLS={'1' if public else '0'}\nGCP_API_PORT={api_port}\n"))
    private_file(config / "api.env", (
        f"DATABASE_URL=postgres://gcp:{db_password}@127.0.0.1:{db_port}/gcp\n"
        "GCP_DATABASE_REQUIRED=1\nGCP_RUN_MIGRATIONS=0\nGCP_DB_MAX_CONNECTIONS=8\n"
        f"GCP_BIND_ADDRESS=127.0.0.1:{api_port}\n"
        f"LIVEKIT_URL={voice_url}\nLIVEKIT_CONTROL_URL=http://127.0.0.1:{signal_port}\n"
        f"LIVEKIT_API_KEY={key}\nLIVEKIT_API_SECRET={secret}\nRUST_LOG=gaming_comms_server=info\n"))
    private_file(config / "init.sql", (
        f"CREATE ROLE gcp LOGIN PASSWORD '{db_password}';\n"
        "ALTER DATABASE gcp OWNER TO gcp;\n"
        "GRANT ALL ON SCHEMA public TO gcp;\n"))
    private_file(config / "livekit.yaml", (
        f"port: {signal_port}\nbind_addresses: [\"127.0.0.1\"]\n"
        f"rtc:\n  node_ip: {media_ip}\n  use_external_ip: false\n"
        f"  enable_loopback_candidate: {'false' if public else 'true'}\n"
        f"  tcp_port: {media_tcp_port}\n  port_range_start: {udp_start}\n"
        f"  port_range_end: {udp_end}\nkeys:\n  {key}: {secret}\n"
        "logging:\n  level: warn\n"))
    # Mounted files need to be readable by container UIDs. The host parent
    # remains 0700; env files stay 0600 and are read only by Compose on the host.
    for name in ("init.sql", "livekit.yaml"):
        (config / name).chmod(0o644)
    if public:
        sites = []
        for hostname, upstream in [(api_domain, api_port), (voice_domain, signal_port)]:
            sites.append(f"{hostname} {{\n"
                         f"  @friends remote_ip {' '.join(networks)}\n"
                         f"  handle @friends {{\n    reverse_proxy 127.0.0.1:{upstream}\n  }}\n"
                         "  handle {\n    respond 403\n  }\n}\n")
        private_file(config / "Caddyfile", "{\n  admin off\n}\n\n" + "\n".join(sites))
        (config / "Caddyfile").chmod(0o644)


class Stack:
    def __init__(self, config):
        self.config = config.resolve(strict=True)
        values = {}
        for line in (self.config / "stack.env").read_text().splitlines():
            if line and not line.startswith("#"):
                key, value = line.split("=", 1)
                values[key] = value
        self.values = values
        self.env = {key: value for key, value in os.environ.items()
                    if key not in values and not key.startswith("COMPOSE_")}
        self.env["GCP_CONFIG_DIR"] = str(self.config)
        self.command = ["docker", "compose", "--env-file", str(self.config / "stack.env"),
                        "--project-name", values["GCP_STACK_PROJECT"], "-f", str(COMPOSE)]
        if values["GCP_ENABLE_TLS"] == "1":
            self.command += ["--profile", "tls"]

    def run(self, *arguments, **kwargs):
        return subprocess.run(self.command + list(arguments), env=self.env, check=True,
                              timeout=1200, **kwargs)

    def database(self, *arguments, **kwargs):
        return self.run("exec", "-T", "postgres", *arguments, **kwargs)

    def backup(self, output):
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(fd, "wb") as archive:
                self.database("pg_dump", "-U", "gcp_admin", "-p", self.values["GCP_DB_PORT"],
                              "-d", "gcp", "--format=custom", "--no-owner", "--no-acl",
                              stdout=archive)
        except BaseException:
            output.unlink()
            raise

    def restore(self, archive):
        # Restore is for a newly initialized project/volume only. Never drop,
        # clean or replace an existing database, even if it contains no users.
        running = self.run("ps", "--status", "running", "-q", "api", "migrate",
                           capture_output=True, text=True).stdout.strip()
        if running:
            raise ValueError("Stop the API and migrations before restoring a new database")
        with archive.open("rb") as source:
            if source.read(5) != b"PGDMP":
                raise ValueError("Expected a trusted pg_dump custom-format archive")
            source.seek(0)
            self.run("up", "-d", "--wait", "--wait-timeout", "120", "postgres")
            count = self.database(
                "psql", "-X", "-U", "gcp_admin", "-p", self.values["GCP_DB_PORT"], "-d", "gcp",
                "-Atc", "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                "WHERE n.nspname='public'", capture_output=True, text=True).stdout.strip()
            if count != "0":
                raise ValueError("Restore refused: target public schema is not empty; use a new project")
            self.database("pg_restore", "-U", "gcp_admin", "-p", self.values["GCP_DB_PORT"],
                          "-d", "gcp", "--role=gcp", "--exit-on-error", "--single-transaction",
                          "--no-owner", "--no-acl", stdin=source)


@contextmanager
def operation_lock(config):
    import fcntl  # Linux host only; leave configuration generation testable elsewhere.
    with (config / "operation.lock").open("a") as lock:
        os.chmod(lock.name, 0o600)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Another operation is using this stack configuration") from None
        yield


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    sub = parser.add_subparsers(dest="action", required=True)
    init = sub.add_parser("init", help="Generate new local credentials; does not start services")
    init.add_argument("--project", default="gcp")
    for name, default in [("api-port", 8080), ("db-port", 5432), ("signal-port", 7880),
                          ("media-tcp-port", 7881), ("udp-start", 50000), ("udp-end", 50100)]:
        init.add_argument("--" + name, type=int, default=default)
    for name in ("api-domain", "voice-domain", "media-ip"):
        init.add_argument("--" + name)
    init.add_argument("--allow-cidr", action="append", default=[], dest="allow_cidrs")
    for name in ("up", "stop", "status"):
        sub.add_parser(name)
    sub.add_parser("backup").add_argument("archive", type=Path)
    sub.add_parser("restore").add_argument("archive", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "init":
            options = vars(args).copy()
            del options["action"]
            initialize(**options)
            print(f"Created configuration in {args.config}. No services started.")
        else:
            if sys.platform != "linux":
                raise ValueError("Run the shared server stack on a Linux Docker Engine host")
            stack = Stack(args.config)
            with operation_lock(stack.config):
                if args.action == "up":
                    stack.run("build", "api")
                    stack.run("up", "-d", "--wait", "--wait-timeout", "180")
                    print("API, PostgreSQL and LiveKit control checks are ready; media is unverified.")
                elif args.action == "stop":
                    stack.run("down", "--timeout", "30")
                elif args.action == "status":
                    stack.run("ps", "--all")
                elif args.action == "backup":
                    stack.backup(args.archive)
                    print(f"Database backup saved to {args.archive}; keep configuration separately.")
                elif args.action == "restore":
                    stack.restore(args.archive)
                    print("Restored into an empty database. Run up to migrate and check readiness.")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        # Commands never include credentials. Do not print the parsed environment.
        parser.exit(1, f"Server stack operation failed: {error}\n")


if __name__ == "__main__":
    main()
