# Gaming Communication Platform

GCP is an experimental Windows x64 communication application for private gaming groups. It uses a C++20/Qt Quick desktop, a Rust/Axum/PostgreSQL API, and a separate native LiveKit media process. It is not yet a finished Discord replacement.

## Current capabilities

| Area | Implemented | Evidence / limits |
| --- | --- | --- |
| Desktop accounts | Sign-in, in-memory access/refresh tokens, timed rotation, local sign-out and best-effort remote logout | Automated API tests and Qt networking tests; no persistent desktop login |
| Communities | API roles, invitations, channels and membership; desktop community/voice-channel selection | PostgreSQL integration tests; creation/invitation administration is API-only |
| Text and friends | Authorized channel messages, friendships, blocks, DMs and text-channel WebSocket events | Backend integration tests; desktop message composer remains disabled |
| Voice | Listen-only, explicit microphone opt-in, mute/F8 push-to-talk, device selection, room events, bounded recovery and Leave | Real SDK integration; managed desktop calls continue until Leave; mute/F8 PTT and periodic access checks; physical audibility unverified |
| Screen sharing | Separate consent-gated monitor diagnostics: CPU-readback publishing and GPU BGRA→NV12→hardware-class H.264→LiveKit bridge | No desktop sharing/viewing UI; hardware, remote playback and driver compatibility unverified |
| Test video | Synthetic RGBA/NV12/BGRA patterns and externally encoded H.264 file publishing | Diagnostic fixtures; not evidence of screen capture or gaming performance |
| Windows ZIP | Qt/QML + voice executable + LiveKit DLL staging, SHA-256 inventory, extracted-ZIP startup checks | Unsigned private test artifact; clean consumer Windows PC still untested |
| Performance | Process CPU/private memory samples, capture/encoder timing, exported game frame-time analysis | No measured gaming resource budget or advantage over other apps established |

See [long-session voice and controls](docs/long-session-voice.md) for the latest voice behavior and tests.

See the [stabilization audit](docs/stabilization-audit.md) for fixes, evidence and remaining gates. Older numbered group documents describe incremental development; use this README and current subsystem guides for present capability boundaries.

## Build Windows desktop

Requirements: Windows 10/11 x64, Visual Studio 2022 C++ workload and Windows SDK, CMake 3.24+, Qt Quick/Quick Controls/Network (Qt 6.8.3 in CI).

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

LiveKit v1.12.2 is opt-in and its downloaded archive is pinned by SHA-256. Without the opt-in flags the Qt shell builds without native voice. See [desktop voice](docs/desktop-voice.md), [hardware streaming diagnostic](docs/hardware-livekit.md), and [development](docs/development.md).

## Backend

```powershell
cargo test --workspace --locked
cargo run -p gaming-comms-server
```

The API binds to `127.0.0.1:8080` by default. `/health` is liveness; `/ready` requires working PostgreSQL. Account, community, message and media-token routes require the documented database configuration; protected operations authenticate database-backed bearer sessions. Database migrations are explicit opt-in. Database integration tests require `GCP_TEST_DATABASE_URL` and run against disposable PostgreSQL in CI; they skip locally when that variable is absent.

Read [PostgreSQL setup](docs/postgresql.md), [authentication](docs/authentication.md), [community roles](docs/communities.md), [messages](docs/messages.md), and [LiveKit authorization](docs/livekit-tokens.md).

For friends-only operation, restrict backend/LiveKit network access to your intended group and use HTTPS/WSS. Account registration is not invite-restricted and there is no complete public-internet abuse protection. The server periodically removes voice participants whose session or channel access has expired or been revoked; see [voice eviction and its limits](docs/server-voice-eviction.md). Keep API signing secrets only on the server.

## Private portable ZIP

After a Release build, put `windeployqt` in PATH:

```powershell
./scripts/package-windows-test.ps1 -BuildDirectory build/desktop/Release -OutputDirectory dist/gcp-windows-test
./scripts/verify-windows-test-bundle.ps1 -BundleDirectory dist/gcp-windows-test
./scripts/package-private-windows-zip.ps1 -BundleDirectory dist/gcp-windows-test -OutputZip dist/gcp-private-windows-x64.zip
```

CI additionally tests staged and extracted Qt/QML startup, native SDK initialization, package corruption rejection and signature inventory consistency. SHA-256 proves consistency with an inventory, not publisher authenticity. No installer, automatic deployment or public release is produced. Publisher signing remains the owner's decision for private distribution. Third-party notices and Qt/SDK licensing need review before sharing binaries; no project license is currently declared.

## Verification boundaries

CI builds Windows media code and tests offline logic. It does not join a live SFU, hear a friend's audio, exercise physical capture/headsets, test a clean consumer PC, or measure gaming CPU/GPU/RAM/FPS. Those acceptance checks remain outstanding. See [physical GPU acceptance](docs/physical-gpu-acceptance.md) and [gaming benchmark](docs/gaming-voice-benchmark.md).
