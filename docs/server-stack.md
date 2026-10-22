# Shared server stack

This package runs the existing Rust API, PostgreSQL and LiveKit together on one
**Linux Docker Engine host**. Only the host runs these services. Friends install
the Windows desktop and connect to the host's API address. The alpha Windows
installer does not install or configure these services.

No remote deployment is performed by this repository change. The automated
acceptance test uses disposable containers with no media peers. A reachable
server, DNS, certificates, firewall/NAT and physical voice testing still need an
actual host. Account registration remains open to anyone who can reach the API;
the remote template restricts requests to the friends' explicitly supplied IP
ranges. Owner bootstrap and invite-only onboarding are the next product group.

## Local bring-up

Use a Linux x64 host with Git, Python 3.10+, Docker Engine and Docker Compose
2.24+ (`docker compose version`). Docker Desktop on Windows and rootless Docker
are not qualified targets for this host-network configuration. Docker access
requires permission to manage the host's Docker daemon.

From the repository root:

```bash
python3 scripts/server_stack.py init
python3 scripts/server_stack.py up
curl --fail http://127.0.0.1:8080/health
curl --fail --include http://127.0.0.1:8080/ready
python3 scripts/server_stack.py status
```

`init` creates random database and LiveKit credentials in
`deploy/self-hosted/.local/`; it refuses to overwrite an existing configuration.
The directory is mode 0700, environment files are 0600, and container-readable
configuration files are inside that private directory. Never commit, publish or
paste these files. For a lasting host, use `--config /srv/gcp/config` **before
each subcommand**, and keep that directory outside a checkout shared with other
users. Back it up separately using encrypted storage.

`up` builds a multi-stage API image using the committed Cargo lockfile, waits
for PostgreSQL, runs embedded migrations as a one-shot service, then starts the
API. The API does not automatically migrate at every startup. It becomes healthy
only after PostgreSQL and an authenticated LiveKit reconciliation pass succeed.
The application connects using a dedicated non-superuser database role; database
initialization and backups use a separate local administrator. This deployment
is a single API/SFU instance; it does not add Redis or distributed routing.

The local configuration advertises `ws://127.0.0.1:7880` and is for checks on the
server itself. It is not a connection address for another PC. Host networking
avoids Docker NAT on the media path. Internal HTTP, signaling and PostgreSQL
listeners bind to loopback; media candidates use the explicitly configured IP.
Keep the host firewall in place for the media ports even during local testing.

The ports can be selected at initialization using `--api-port`, `--db-port`,
`--signal-port`, `--media-tcp-port`, `--udp-start` and `--udp-end`. Use separate
ports and project names for multiple stacks on one host. Changing a database
password in an environment file does not change a role in an existing volume.

## Remote host configuration

Prepare two DNS names pointing directly to the host, a reachable media IPv4
address, and the actual source CIDRs of your friends. Then generate a **new**
configuration, for example:

```bash
python3 scripts/server_stack.py --config /srv/gcp/config init \
  --project gcp-friends \
  --api-domain api.example.com --voice-domain voice.example.com \
  --media-ip 203.0.113.20 \
  --allow-cidr 198.51.100.10/32 --allow-cidr 198.51.100.11/32
python3 scripts/server_stack.py --config /srv/gcp/config up
```

Replace every example address. Caddy obtains publicly trusted certificates and
proxies HTTPS/WSS to loopback. It persists certificate state in named volumes.
The API advertises the public WSS address, while `LIVEKIT_CONTROL_URL` points to
loopback for internal authenticated control calls. That override permits HTTPS
or literal-loopback HTTP only; remote plaintext signaling remains rejected.
The generated Caddy configuration uses the connection's source IP, not a
user-supplied forwarding header. Use direct DNS records; placing a CDN or HTTP
proxy in front changes the source-IP policy and requires separate review.

Configure the host firewall and, if applicable, router forwarding/cloud rules:

| Port | Allowed source / purpose |
| --- | --- |
| TCP 80 | Certificate HTTP challenges and HTTPS redirects; must remain reachable for renewal |
| TCP 443 | Friends' source ranges; API and LiveKit WSS |
| TCP 7881 | Friends' source ranges; encrypted WebRTC TCP fallback |
| UDP 50000–50100 | Friends' source ranges; encrypted WebRTC media |
| TCP 5432, 8080, 7880 | Loopback only; never forward or expose |

Custom ports must be reflected in firewall rules. Caddy's HTTPS proxy does not
carry UDP media. Behind NAT, `--media-ip` must be the reachable forwarded address;
local clients may need router hairpin support. A VPN setup can use a reachable
VPN IPv4 address and friends' VPN CIDRs, with trusted TLS and DNS still required.
Dynamic friend IPs require updating the source policy and firewall. The package
does not configure TURN; networks blocking both direct UDP and ICE/TCP remain a
connectivity limitation. The explicit IPv4 configuration is deliberate; IPv6,
multi-host operation and restrictive-network relay support are not qualified.
LiveKit's upstream [deployment](https://docs.livekit.io/transport/self-hosting/deployment/)
and [port reference](https://docs.livekit.io/transport/self-hosting/ports-firewall/)
explain TLS and direct-media requirements.

From an allowed friend's PC, check `https://api.example.com/ready` (204), then
use `https://api.example.com` in the desktop. Real audio acceptance is separate:
two accounts, separate PCs/headsets, listen-only and microphone consent, mute,
PTT, reconnect and Leave. No physical two-user results are implied by startup.

## Restart, updates and readiness

```bash
python3 scripts/server_stack.py --config /srv/gcp/config stop
python3 scripts/server_stack.py --config /srv/gcp/config up
```

`stop` removes containers and retains named database/certificate volumes.
Long-running services use `unless-stopped` restart behavior when Docker restarts.
Enable the Docker service at boot on the host if desired. The API handles
SIGINT/SIGTERM and Docker allows 30 seconds for connections to drain; active
calls and subscriptions should be expected to reconnect after maintenance.
Health status is not an automatic container restart policy: dependency failures
make `/ready` return 503, while a running API keeps `/health` live. Existing
database pools and the LiveKit reconciliation loop recover when services return.

Take a backup before updating. Review the target commit and migrations, stop the
stack, update the checkout, and run `up`. Applied migration checksums are checked
by SQLx. There is no automatic migration downgrade. Restore into a separate stack
with the matching application version when a rollback is necessary. Retain the
old stack and backup until recovery is verified. A database major-version change
requires an explicit PostgreSQL upgrade plan; never point a different major at
an old data volume.

Image versions are explicit in Compose/Dockerfile (Rust 1.99.1, PostgreSQL 16.15,
LiveKit 1.13.10 and Caddy 2.11.7). The minimal Debian runtime tracks Bookworm
security updates. Version tags and apt packages are not immutable image digests;
this is a buildable source package, not a bit-reproducible image distribution.
Review upstream patches and rerun acceptance when updating these pins. No
container registry publication is part of this workflow. Logs rotate locally;
API and SFU containers run without root or Linux capabilities. No media quality
settings or client resource-performance claims are changed by packaging.

## Backups and restoration

```bash
python3 scripts/server_stack.py --config /srv/gcp/config backup /secure/gcp-2026-10-22.pgdump
```

This creates a consistent PostgreSQL custom-format dump, mode 0600, and refuses
to replace an existing file. A failed dump removes its partial output. Backups
contain private messages, account password hashes and session digests. Encrypt
them, restrict access and retain the matching application commit. Configuration
secrets and Caddy certificate volumes are **not** included. Keep an encrypted
copy of the configuration separately; media rooms are ephemeral and need no
backup. Establish an off-host backup schedule and retention policy on the actual
host. Never load an untrusted SQL archive: restoration executes SQL.

Restore to a new, empty project and volume, using distinct ports if the source
stack is still running:

```bash
python3 scripts/server_stack.py --config /srv/gcp-recovery/config init --project gcp-recovery \
  --api-port 18080 --db-port 15432 --signal-port 17880 --media-tcp-port 17881 \
  --udp-start 51000 --udp-end 51100
python3 scripts/server_stack.py --config /srv/gcp-recovery/config restore /secure/gcp-2026-10-22.pgdump
python3 scripts/server_stack.py --config /srv/gcp-recovery/config up
curl --fail --include http://127.0.0.1:18080/ready
```

The restore tool refuses a running API/migration process and any populated public
schema. It does not drop tables, clean a database or delete operator volumes.
The new project gets new database/SFU secrets, while restored account passwords
and data remain valid. Sessions are restored to their snapshot state; sessions
revoked after the backup can become valid again. Revoke restored sessions before
reopening access when that matters. Verify login and persisted community/channel
data before selecting a recovery host. Use a remote-mode configuration at `init`
if this recovery project will serve friends. Do not delete the original volumes
until the owner has accepted recovery.

## Automated evidence

```bash
python3 -m unittest discover -s scripts -p 'test_server_stack.py'
python3 scripts/verify_server_stack.py
```

The second command needs Docker, builds the real API and creates disposable
randomly named projects. It verifies fresh migrations, account/login and role
protected API operations, signed microphone grants against the configured SFU
keys, real LiveKit room-control authentication, dependency loss/recovery,
SIGTERM exit, public-WSS/private-control separation, persistence across container
recreation, refusal to overwrite a populated database, restoration into a fresh
volume with new service credentials, Caddy configuration validation, actual TLS
proxy requests using an explicitly trusted disposable local CA, and source-IP
access denial. Public certificate issuance is not exercised. It
deletes only its generated test projects and volumes. Ports 18080, 15432, 17880,
17881, 18443, 18444 and UDP 17900–17920 must be free; use a dedicated test host/CI runner.

The `Self-hosted stack acceptance` CI job runs this contract alongside Rust and
Windows checks and gates future configured preview publication. It does not
connect media peers, publish certificates, deploy a service, hear audio, test
screen sharing or measure gaming performance.
