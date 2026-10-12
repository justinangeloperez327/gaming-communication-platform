# Gaming Communication Platform

A Windows-first communication client for gaming communities and teams. The long-term product includes text channels, low-latency voice, screen sharing, and hardware-accelerated game streaming with measurable CPU, GPU, and RAM efficiency.

## Current implementation

- **Desktop:** C++20, Qt 6, and Qt Quick. A runnable native desktop shell with local interface interactions.
- **Server:** Rust (edition 2024), Tokio, and Axum. A runnable HTTP process with a health endpoint and a regression test.
- **CI:** Manual and pull-request-triggered Rust checks and Windows desktop builds.
- **Not implemented yet:** real accounts, messaging, voice, capture, WebRTC, streaming, and persistent storage. UI examples are illustrative only.

## Requirements

- Windows 10/11, Visual Studio 2022 C++ toolchain, CMake 3.24+, Qt 6.5+ with Qt Quick Controls 2 (6.8.3 is used in CI).
- Rust stable supporting the 2024 edition (1.85+).
- Qt must be built for the compiler you are using, such as MSVC 2022 x64.

## Run the desktop client

In a Visual Studio Developer PowerShell at the repository root:

1. Configure: `cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"`
2. Build: `cmake --build build/desktop --config Release --parallel`
3. Test: `ctest --test-dir build/desktop -C Release --output-on-failure`
4. Run: `build/desktop/apps/desktop/Release/gcp-desktop.exe` (Qt DLLs/plugins must be available; run from a Qt-enabled environment or deploy with windeployqt).

Adjust the Qt path to your installed version and architecture. The desktop UI currently runs independently of the server.

## Run the backend

1. Start: `cargo run -p gaming-comms-server`
2. Verify: `curl http://127.0.0.1:8080/health`
3. Test: `cargo test --workspace`
4. Lint: `cargo fmt --all -- --check` and `cargo clippy --workspace --all-targets -- -D warnings`

By default, the API listens on loopback only. Set `GCP_BIND_ADDRESS` explicitly if a different listener is required. The only implemented route is `GET /health`.

## Engineering approach

The native client and Rust backend are separate programs. Media integration will be validated through a narrow capture/encode/transport prototype before implementing production streaming. See [Architecture](docs/architecture.md), [performance criteria](docs/performance.md), and [development setup](docs/development.md).

This repository does not yet include an open-source license. Confirm licensing and third-party distribution obligations before publishing packaged binaries.
