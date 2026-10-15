# Gaming Communication Platform

A Windows-first communication client for gaming communities and teams. The long-term product includes text channels, low-latency voice, screen sharing, and hardware-accelerated game streaming with measurable CPU, GPU, and RAM efficiency.

## Currently available

- **Desktop:** C++20, Qt 6 and Qt Quick native shell with local UI interactions.
- **Backend:** Rust, Tokio, Axum, PostgreSQL migrations and server-side account authentication using Argon2id and revocable opaque sessions.
- **Windows media diagnostic:** Windows Graphics Capture monitor frame counting, WASAPI microphone packet counting, DXGI GPU inventory, Media Foundation encoder candidate inventory, real synthetic-frame H.264 encode benchmark, and CPU BGRA copy benchmark.
- **Optional LiveKit diagnostic:** v1.12.2 Windows C++ SDK with synthetic video and opt-in actual monitor capture publishing (requires LiveKit tokens and a Windows interactive desktop).
- **Performance instrumentation:** bounded capture buffers, CPU/RAM metrics and p50/p95 staging-readback samples; one redundant full-frame CPU copy removed. GPU-resident H.264 encoding and confirmed hardware-encoder selection are not yet implemented.
- **Hardware-video feasibility:** hardware-class H.264 Media Foundation MFT activation diagnostic, plus opt-in publishing of **externally encoded** Annex B H.264 video through LiveKit. Neither feature encodes frames inside the application.
- **Actual offline H.264 encode:** Media Foundation synthetic NV12 test frames are encoded to a temporary MP4 using hardware transforms when the OS supports them; **actual hardware selection is not verified**.
- **GPU pipeline qualification:** D3D11 hardware video device, NV12 GPU texture/media sample, and hardware-only MFT format negotiation, without transmitting pixels or encoding frames. See [GPU qualification](docs/gpu-h264-qualification.md).
- **Async hardware-encoder probe:** a separate Windows test attempts to encode synthetic D3D11/NV12 frames with event-driven, hardware-class Media Foundation H.264 transforms. It also supports a GPU-side **BGRA → NV12** video-processing path (`--source bgra`) without GPU readback. Runs only on physical hardware; does not capture a real screen or publish a live stream. See [hardware encoding](docs/hardware-h264.md).
- **Opt-in monitor GPU encoding probe:** explicitly selected Windows display frames can now be passed to D3D11 BGRA→NV12 conversion and a hardware-class H.264 MFT using a shared D3D11 device. This is a local, physical-hardware-only test and does not transmit screen content. See [live monitor diagnostic](docs/live-monitor-encode.md).
- **Experimental hardware-to-LiveKit bridge:** an opt-in Windows developer diagnostic connects monitor capture, GPU NV12 conversion, hardware-class H.264 MFT output and the LiveKit encoded-video source. Requires actual GPU/room testing; not a production feature. See [hardware streaming](docs/hardware-livekit.md).
- **Encoder compatibility and feedback:** optional hardware-to-LiveKit diagnostic can recover valid SPS/PPS from Media Foundation H.264 metadata and attempt keyframe/bitrate controls. Driver acceptance is not proof of delivery or adaptation; see [hardware-to-LiveKit](docs/hardware-livekit.md).
- **Streaming measurements:** receiver decoded-callback timing, bounded sender H.264 output/packet timing, process CPU/private memory, and a repeatable baseline-vs-streaming game frame-time CSV tool. None of these proves actual remote playback without a real two-client test or measures GPU engine use. See [validation protocol](docs/stream-validation.md).
- **Bounded stability session (experimental):** the native hardware monitor and LiveKit diagnostics support explicit `--duration` up to 300 seconds, with capped frame submission and idle-screen handling. Physical-GPU execution and decoded playback remain unverified. See [stream validation](docs/stream-validation.md).
- **Physical-GPU acceptance toolkit:** Windows Video Encode counter sampling plus an offline, fail-closed sender/receiver diagnostic evidence checker. The toolkit does not itself certify hardware use. See [Group 16 runbook](docs/physical-gpu-acceptance.md).
- **CI:** Rust verification and Windows native build/CTest checks.

**Not implemented:** working messaging, community permission enforcement, production voice chat, screen-sharing UI, game window selection, and production hardware encoding control. Registration/login APIs exist, but email verification, distributed abuse controls, and production security hardening remain outstanding. A developer-only monitor publisher exists but uses CPU readback and is not optimized game streaming.

## Requirements

- Windows 10/11 x64, Visual Studio 2022 C++ workload, Windows SDK, CMake 3.24+.
- Qt 6.5+ with Qt Quick Controls 2 (6.8.3 in CI).
- Rust stable 1.85+ for the 2024 edition.

## Desktop and native capture diagnostic

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
.\build\desktop\Release\gcp-media-probe.exe --help
```

Run `build/desktop/Release/gcp-desktop.exe` in a Qt-enabled environment. For capture tests and limits, see [Windows media probe](docs/media-probe.md) and [performance measurements](docs/performance.md).

To **opt in** to the LiveKit SDK, see [LiveKit smoke test](docs/livekit-smoke.md). For pre-encoded H.264 publishing and hardware-class MFT diagnostics, see [H.264 integration](docs/h264-integration.md). Real monitor publishing is documented in [monitor sharing prototype](docs/monitor-share.md).

## Rust backend

```powershell
cargo run -p gaming-comms-server
# separate terminal:
curl http://127.0.0.1:8080/health
cargo test --workspace
```

By default the backend binds to loopback. `GET /health` is liveness and remains available without a database; `GET /ready` returns 204 only when PostgreSQL is connected and responsive. The server does not expose user/community/message CRUD endpoints yet.

For PostgreSQL setup, migration controls and database testing, see [PostgreSQL foundation](docs/postgresql.md). See [authentication API](docs/authentication.md) for accounts and persistent sessions, and [communities and channels](docs/communities.md) for role-checked membership, channel management and invitations. See [text messaging](docs/messages.md) for permission-checked channel messages and pagination.

## Architecture and performance

See [architecture](docs/architecture.md), [performance acceptance criteria](docs/performance.md), [native H.264 encoding benchmark](docs/native-h264-encode.md), and [Windows setup](docs/development.md).

There is no project license yet. Third-party and Qt licensing must be reviewed before distributing binaries.

- **Experimental real-time events:** authenticated single-process text-channel WebSocket notifications, typing, ephemeral presence, and reconnect via HTTP message history. See [real-time events](docs/realtime.md). No cross-instance delivery guarantee.

- **Friendships and direct messages:** PostgreSQL-backed invitations, owner-controlled blocks and permission-checked private text conversations. See [friends and DMs](docs/friends-dm.md). No DM E2EE or real-time DM socket yet.

- **LiveKit voice authorization (experimental):** short-lived, server-signed room credentials for authorized community voice-channel members. Microphone-only grants; not a full desktop voice client or active SFU revocation. See [voice token API](docs/livekit-tokens.md).

- **Native voice diagnostic (Group 27):** consent-gated WebRTC microphone publishing, listen-only speaker mode, device selection, and mute test. Requires a two-user physical Windows audio test. See [native voice](docs/native-voice.md).
