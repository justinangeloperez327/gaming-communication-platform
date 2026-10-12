# Gaming Communication Platform

A Windows-first communication client for gaming communities and teams. The long-term product includes text channels, low-latency voice, screen sharing, and hardware-accelerated game streaming with measurable CPU, GPU, and RAM efficiency.

## Current implementation

- **Desktop:** C++20, Qt 6, and Qt Quick. A runnable native desktop shell with local interface interactions.
- **Server:** Rust (edition 2024), Tokio, and Axum. A runnable HTTP process with a health endpoint and regression tests.
- **Native media feasibility:** A separate Windows diagnostic CLI for monitor enumeration, GPU-backed frame acquisition with Windows Graphics Capture, audio endpoint discovery and event-driven WASAPI microphone packet counting. It prints basic CPU/memory metrics, without recording or uploading audio/video.
- **CI:** Pull-request Rust checks and Windows Qt/native build plus smoke tests.
- **Not implemented yet:** real accounts, messaging, voice calls, video encoding, WebRTC, streaming, and persistent storage.

## Requirements

- Windows 10/11, Visual Studio 2022 C++ toolchain, Windows SDK, CMake 3.24+, Qt 6.5+ with Qt Quick Controls 2 (6.8.3 in CI).
- Rust stable supporting the 2024 edition (1.85+).
- Qt installed for the same compiler, such as MSVC 2022 x64.

## Run the desktop and diagnostic

In a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
.\build\desktop\native\probe\Release\gcp-media-probe.exe --help
```

The Qt desktop executable is `build/desktop/Release/gcp-desktop.exe`; Qt runtime DLLs/plugins must be available. See [media probe instructions](docs/media-probe.md) for **real hardware** monitor and microphone testing.

## Run the backend

```powershell
cargo run -p gaming-comms-server
# In another terminal:
curl http://127.0.0.1:8080/health
cargo test --workspace
```

By default, the API listens on loopback only. Set `GCP_BIND_ADDRESS` explicitly if required. The only implemented HTTP route is `GET /health`.

## Engineering approach

The native client and Rust backend are separate programs. Media integration will be validated through capture/encode/transport tests before production streaming. See [Architecture](docs/architecture.md), [performance criteria](docs/performance.md), and [development setup](docs/development.md).

No repository license is included yet. Confirm Qt and third-party distribution obligations before publishing binaries.
