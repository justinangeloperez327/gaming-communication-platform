# Gaming Communication Platform

A Windows-first communication client for gaming communities and teams. The long-term product includes text channels, low-latency voice, screen sharing, and hardware-accelerated game streaming with measurable CPU, GPU, and RAM efficiency.

## Currently available

- **Desktop:** C++20, Qt 6 and Qt Quick native shell with local UI interactions.
- **Backend:** Rust, Tokio and Axum with a health endpoint and tests.
- **Windows media diagnostic:** Windows Graphics Capture monitor frame counting, WASAPI microphone packet counting, basic CPU and private-memory measurement.
- **Optional LiveKit diagnostic:** v1.12.2 Windows C++ SDK with synthetic video and opt-in actual monitor capture publishing (requires LiveKit tokens and a Windows interactive desktop).
- **CI:** Rust verification and Windows native build/CTest checks.

**Not implemented:** authenticated accounts, real messaging, production voice chat, screen-sharing UI, game window selection, hardware encoding control and persistent storage. A developer-only monitor publisher exists but uses CPU readback and is not optimized game streaming.

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

Run `build/desktop/Release/gcp-desktop.exe` in a Qt-enabled environment. For capture tests and limits, see [Windows media probe](docs/media-probe.md).

To **opt in** to the LiveKit SDK and synthetic room test, see [LiveKit smoke test](docs/livekit-smoke.md). Real monitor publishing is documented in [monitor sharing prototype](docs/monitor-share.md).

## Rust backend

```powershell
cargo run -p gaming-comms-server
# separate terminal:
curl http://127.0.0.1:8080/health
cargo test --workspace
```

By default the backend binds to loopback. The only implemented HTTP route is `GET /health`.

## Architecture and performance

See [architecture](docs/architecture.md), [performance acceptance criteria](docs/performance.md), and [Windows setup](docs/development.md).

There is no project license yet. Third-party and Qt licensing must be reviewed before distributing binaries.
