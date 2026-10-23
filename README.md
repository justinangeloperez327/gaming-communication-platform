# Gaming Communication Platform

GCP is an experimental Windows x64 communication application for private gaming groups. It uses a C++20/Qt Quick desktop, a Rust/Axum/PostgreSQL API, and a separate native LiveKit media process. It is not yet a finished Discord replacement.

## Current capabilities

| Area | Implemented | Evidence / limits |
| --- | --- | --- |
| Desktop accounts | Invited account creation, owner invitation controls, sign-in, in-memory tokens, timed rotation and sign-out | PostgreSQL, real-stack and Qt tests; no email delivery/recovery or persistent login |
| Communities | Desktop create/join/leave, channel editing, invitations and role-aware member administration; API ownership enforcement | PostgreSQL, container and Qt tests; capped lists, no live directory updates or owner transfer |
| Text | Desktop paginated history, plain-text send/edit/delete, independent text/voice selection and live reconnect recovery | Qt/QML/HTTP/WS, real Qt→Rust→PostgreSQL and retry/authorization tests; physical two-PC acceptance pending |
| Friends and DMs | Desktop exact-handle requests, accept/decline/cancel, block/unblock, private history/composer and live DM recovery | Shipped Qt controls, real Qt→Rust→PostgreSQL and authorization/socket tests; single-process events, no physical two-PC acceptance |
| Voice | Listen-only, per-call mic consent, mute/F8 PTT, device selection, muted recovery with immediate access recheck, and Leave | Qt/QProcess policy tests and real loopback synthetic PCM transport/mute/isolation/rejoin/active-eviction contract; physical audibility unverified |
| Screen sharing | Separate consent-gated monitor diagnostics: CPU-readback publishing and GPU BGRA→NV12→hardware-class H.264→LiveKit bridge | No desktop sharing/viewing UI; hardware, remote playback and driver compatibility unverified |
| Test video | Synthetic RGBA/NV12/BGRA patterns and externally encoded H.264 file publishing | Diagnostic fixtures; not evidence of screen capture or gaming performance |
| Windows distribution | Qt/QML + voice executable + LiveKit DLL staging, portable ZIP, per-user alpha installer, SHA-256 inventory and installed/extracted startup checks | Unsigned preview; clean consumer Windows PC still untested |
| Performance | Process CPU/private memory samples, capture/encoder timing, exported game frame-time analysis | No measured gaming resource budget or advantage over other apps established |

See [long-session voice and controls](docs/long-session-voice.md) for the latest voice behavior and tests.
Group 4's [voice evidence and acceptance guide](docs/usable-voice.md) separates
synthetic media checks from the owner's pending two-PC test.
Use [desktop communities](docs/desktop-communities.md) to create a private space,
add channels and invite existing accounts. See [desktop messaging](docs/desktop-messaging.md)
for text history, send retries, live updates and their limits.
[Friends and private messages](docs/desktop-friends.md) adds separate private conversations without changing voice selection.

The ZIP includes a [versioned notice baseline](docs/windows-third-party-notices.md); complete transitive attribution review remains pending.

See the [current stabilization audit](docs/stabilization-status.md) for completed fixes, verified functionality, CI evidence and remaining installer blockers. The [15 October audit](docs/stabilization-audit.md) and older numbered group documents are historical; use this README and current subsystem guides for present capability boundaries.

## Build Windows desktop

Requirements: Windows 10/11 x64, Visual Studio 2022 C++ workload and Windows SDK, CMake 3.24+, Qt Quick/Quick Controls/Network/**WebSockets** (Qt 6.8.3 in CI).

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

LiveKit v1.12.2 is opt-in and its downloaded archive is pinned by SHA-256. Without the opt-in flags the Qt shell builds without native voice. See [desktop voice](docs/desktop-voice.md), [hardware streaming diagnostic](docs/hardware-livekit.md), and [development](docs/development.md).

## Backend

For the shared Linux host, use the [server stack package](docs/server-stack.md):
generated credentials, PostgreSQL + API + LiveKit, migrations, optional restricted
HTTPS/WSS, persistence and backup/restore. Friends only need the Windows desktop.
See the [completion roadmap](docs/completion-roadmap.md) for the remaining product
groups and acceptance requirements.

```powershell
cargo test --workspace --locked
cargo run -p gaming-comms-server
```

The API binds to `127.0.0.1:8080` by default. `/health` is liveness; `/ready` requires working PostgreSQL. Account, community, message and media-token routes require the documented database configuration; protected operations authenticate database-backed bearer sessions. Database migrations are explicit opt-in. Database integration tests require `GCP_TEST_DATABASE_URL` and run against disposable PostgreSQL in CI; they skip locally when that variable is absent.

Read [PostgreSQL setup](docs/postgresql.md), [authentication](docs/authentication.md), [community roles](docs/communities.md), [messages](docs/messages.md), and [LiveKit authorization](docs/livekit-tokens.md).

For friends-only operation, configure the owner on the shared host and use [private account invitations](docs/account-onboarding.md). Restrict backend/LiveKit network access to your intended group and use HTTPS/WSS; complete public-internet abuse protection remains absent. The server periodically removes voice participants whose session or channel access has expired or been revoked; see [voice eviction and its limits](docs/server-voice-eviction.md). Keep API signing secrets only on the server.

## Private portable ZIP

After a Release build, put `windeployqt` in PATH:

```powershell
./scripts/package-windows-test.ps1 -BuildDirectory build/desktop/Release -OutputDirectory dist/gcp-windows-test
./scripts/verify-windows-test-bundle.ps1 -BundleDirectory dist/gcp-windows-test
./scripts/package-private-windows-zip.ps1 -BundleDirectory dist/gcp-windows-test -OutputZip dist/gcp-private-windows-x64.zip
```

CI additionally tests staged and extracted Qt/QML startup, native SDK initialization, package corruption rejection and signature inventory consistency. The owner-requested [alpha installer and prerelease](docs/windows-installer.md) add per-user installation, repair/uninstall tests and CI-gated publication of the configured version. Downloads include the desktop runtime; backend/PostgreSQL/LiveKit services remain separate. SHA-256 proves consistency, not publisher authenticity. Publisher signing remains the owner's responsibility. Complete third-party attribution review remains open; no project license is currently declared.

## Verification boundaries

CI runs actual Qt channel/friend/private-message controllers against Rust/PostgreSQL and exercises shipped
QML with controlled network failures. It also builds Windows media code and runs
a real loopback LiveKit synthetic-audio
contract: room joins, decoded PCM, mute/unmute, isolation, explicit rejoin and
active publisher eviction. The Rust control contract and stack tests run
separately. CI does not hear a friend's audio, exercise physical capture/headsets,
force actual SDK network interruption, test a clean consumer PC, or measure gaming
CPU/GPU/RAM/FPS. Those checks remain outstanding. See [voice acceptance](docs/usable-voice.md),
[physical GPU acceptance](docs/physical-gpu-acceptance.md) and [gaming benchmark](docs/gaming-voice-benchmark.md).
