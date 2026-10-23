# Development

## Windows desktop

Install Visual Studio 2022 with Desktop development with C++, Qt 6 (MSVC 2022 x64, Qt Quick Controls and Qt WebSockets), Windows SDK and CMake 3.24 or newer.

From a Developer PowerShell:

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

Use your actual Qt install path. Qt DLLs and QML plugins must be discoverable when running outside Qt Creator. For future packaging, deploy the app with `windeployqt` and review Qt licensing.

The separate Windows native media diagnostic is built in `build/desktop/Release/gcp-media-probe.exe`. See [media probe](media-probe.md) for commands and limitations.

## Rust HTTP server

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo run -p gaming-comms-server
```

Open `http://127.0.0.1:8080/health`; expect `{"service":"gaming-comms-server","status":"ok"}`.

To bind a different address in PowerShell: `$env:GCP_BIND_ADDRESS="127.0.0.1:9000"`.

## CI and contribution workflow

GitHub Actions CI runs for pull requests, main pushes and manual dispatch. CI compiles the Windows media probe and tests CLI primitives but does **not** execute actual microphone/monitor capture on GitHub-hosted runners. CI stages and uploads test ZIP artifacts and runs a real loopback synthetic-audio SDK/SFU contract; see [voice evidence](usable-voice.md). PR checks do not publish releases or deploy services. The existing main-branch alpha publication is version-gated; Group 5 does not change its version or release configuration. Tests do not change production databases.

Maintain isolated feature branches. Build and test locally before merging. Keep documentation about existing capabilities accurate; planned functionality belongs in issues/PR descriptions rather than pretending to exist in the README.


The separate Linux **Qt to Rust messaging acceptance** job compiles the real
AccountSession, ChannelDirectory, ChatSession and MessageModel without the GUI or
media SDK, then runs two disposable accounts through the actual API/PostgreSQL
and channel sockets. Its `scripts/verify_desktop_chat.py` runner explicitly
migrates and bootstraps only a loopback database named `gcp_chat_test`; it is not
an operator tool for an existing service. The Windows `desktop_text_messaging`
CTest exercises the shipped ChatPanel with controlled reply loss, socket gaps,
malformed/oversized replies and actual access-token rotation. See
[desktop messaging](desktop-messaging.md) for behavior and limits.
