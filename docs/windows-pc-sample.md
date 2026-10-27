# Host a sample on your Windows PC

This is a **same-PC, localhost-only** sample of the real Rust API, PostgreSQL
and LiveKit. It uses the existing application, migrations and invitation flow;
it does not seed fake accounts/messages or enable microphone/screen consent.
Friends on another PC cannot connect to this localhost address.

## Before starting

Install Git, Python 3.10+ (with the Windows `py` launcher), and Docker Desktop
using its WSL2 backend and **Linux containers**. Start Docker Desktop. Check:

```powershell
py --version
docker version
docker compose version
```

Use Docker Compose 2.24+ and an x64 PC. Docker needs internet access for the first
image download/build. Allow time for the first Rust compilation. Keep Docker
running while using the sample. This configuration does not need Docker Desktop
host networking, router forwarding, a public domain or a TLS certificate.

Run from the repository root in PowerShell:

```powershell
py scripts/pc_sample.py init
py scripts/pc_sample.py up
py scripts/pc_sample.py owner --email you@example.com --handle owner --display-name "Server owner"
```

Replace the email/name with yours. The final command prompts privately for a
password of 12–128 UTF-8 bytes and confirmation. This email is an account
identifier: the sample does not send email. Run owner bootstrap once. The
configuration generator refuses to replace existing secrets.

## Connect the desktop

Open an up-to-date GCP Windows desktop build and enter this server address:

```text
http://127.0.0.1:18080
```

Sign in with the owner account. Create a community/channel and use the owner's
account invitation controls to invite another email if you want another test
account. Invitation codes are provided through the app, not sent as email.
For the actual controls see [private onboarding](account-onboarding.md).

Check readiness from PowerShell:

```powershell
curl.exe --fail --include http://127.0.0.1:18080/ready
py scripts/pc_sample.py status
```

Readiness returns HTTP 204. It verifies API/database/SFU control readiness;
it does not prove audibility, Windows media routing or capture quality. Start
voice, microphone publication, screen sharing and viewing only through their
explicit desktop controls. Never interpret the local sample as a two-PC test.

## Stop, restart and back up

```powershell
py scripts/pc_sample.py stop
py scripts/pc_sample.py up
py scripts/pc_sample.py backup "$env:USERPROFILE\gcp-sample-backup.pgdump"
```

Stop retains the database volume. Backup refuses to overwrite an existing file.
It includes private account/message data: keep it private and keep a separate
private copy of `deploy/self-hosted/.pc-local/`. That ignored config folder
contains generated credentials; do not share or commit it. On Windows, keep the
checkout/config under your own user profile with normal user-only access; POSIX
permission bits do not establish Windows ACL protection. Reuse the same folder
and Compose project to retain the database; do not regenerate passwords over an
existing volume. Restore uses the existing Linux-host recovery workflow and is
not exposed by this small sample launcher.

## Ports and limits

The sample publishes only `127.0.0.1`: TCP 18080 (API), 17880 (signaling), 17881
(media fallback) and UDP 17900–17920. PostgreSQL 15432 stays inside the shared
container network namespace and has no published host port. This is separate
from the regular host stack's project and config. Only one PC sample project
may run per Docker daemon. Stop it before changing a sample configuration.
Do not manually change mappings to `0.0.0.0` or replace localhost with a LAN IP:
the current client accepts cleartext only for localhost, and the SFU advertises
loopback candidates. Use the [remote-host setup](server-stack.md) for friends,
with trusted HTTPS/WSS and reachable media networking.

Configuration and lock-policy tests run offline on Linux and Windows. The
container acceptance checks real readiness, bootstrap/login, loopback port
mappings and persistence on Linux Docker. Docker Desktop/WSL2 forwarding and
physical voice/screen operation must still be checked on your Windows PC.
If a port is occupied, stop the conflicting application instead of altering
credentials. If `/ready` fails, run `status`; share the service state and error
message, never the generated environment files or full rendered Compose config.
