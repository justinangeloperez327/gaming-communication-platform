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

GitHub Actions CI runs for pull requests, main pushes and manual dispatch. CI compiles the Windows media probe and tests CLI primitives but does **not** execute actual microphone/monitor capture on GitHub-hosted runners. CI stages and uploads test ZIP artifacts and runs a real loopback synthetic-audio SDK/SFU contract; see [voice evidence](usable-voice.md). PR checks do not publish releases or deploy services. The existing main-branch alpha publication is version-gated; Group 8 does not change its version or release configuration. Tests do not change production databases.

Maintain isolated feature branches. Build and test locally before merging. Keep documentation about existing capabilities accurate; planned functionality belongs in issues/PR descriptions rather than pretending to exist in the README.


The separate Linux **Qt to Rust messaging and screen lease acceptance** job compiles the real
AccountSession, ChannelDirectory, ChatSession and MessageModel without the GUI or
media SDK, then runs two disposable accounts through the actual API/PostgreSQL,
channel sockets, friend requests, private sockets and bilateral revocation. Its `scripts/verify_desktop_chat.py` runner explicitly
migrates and bootstraps only a loopback database named `gcp_chat_test`; it is not
an operator tool for an existing service. The Windows `desktop_text_messaging`
CTest exercises the shipped ChatPanel with controlled reply loss, socket gaps,
malformed/oversized replies and actual access-token rotation. See
[desktop messaging](desktop-messaging.md) for behavior and limits.

The `desktop_friends_and_private_scope` CTest loads the shipped FriendsPanel and
covers staged refreshes, action direction, malformed/oversized responses,
cancellation/destruction, peer validation and a refresh-timer 401 race. The
complete `desktop_qml_startup` test runs the real Main window before deployment;
`--smoke-qml` sends startup diagnostics to captured stderr for GUI builds.

Group 7 adds a PostgreSQL-backed screen lease API test to `livekit_eviction.rs`.
The Windows synthetic-media runner now also runs `gcp-livekit-screen-contract`
using the existing pinned SDK and disposable SFU. It checks role/source grants,
decoded synthetic pixels, separate voice-room isolation, exact track ownership
and publisher-only removal. Its fixture tokens are independent of the Rust
API tests; no physical capture or desktop viewer runs. See
[screen authorization](screen-authorization.md) for verification boundaries and
the explicit migration/desktop integration contract.

Group 8 adds `desktop_screen_consent_and_publisher_lifecycle` and native
screen-control/H.264 self-tests. The Linux acceptance command now also requires
`--screen-fixture build/chat-api/gcp-screen-process-fixture`; it starts the same
checksum-pinned SFU for real control readiness and Rust-issued screen leases.
The test-only native peer never captures or transmits media. The Windows
publisher remains subject to the physical acceptance in
[desktop screen publisher](desktop-screen-publisher.md).
