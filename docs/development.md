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

GitHub Actions CI runs for pull requests, main pushes and manual dispatch. CI compiles the Windows media probe and tests CLI primitives but does **not** execute actual microphone/monitor capture on GitHub-hosted runners. CI stages and uploads test ZIP artifacts and runs a real loopback synthetic-audio SDK/SFU contract; see [voice evidence](usable-voice.md). PR checks do not publish releases or deploy services. The existing main-branch alpha publication is version-gated; Groups 8–10 do not change its version or release configuration. Tests do not change production databases.

Maintain isolated feature branches. Build and test locally before merging. Keep documentation about existing capabilities accurate; planned functionality belongs in issues/PR descriptions rather than pretending to exist in the README.


The separate Linux **Qt to Rust messaging and screen lease acceptance** job compiles the real
AccountSession, ChannelDirectory, ChatSession and MessageModel without the
media SDK, then runs two disposable accounts through the actual API/PostgreSQL,
channel sockets, friend requests, private sockets and bilateral revocation. Its `scripts/verify_desktop_chat.py` runner explicitly
migrates and bootstraps only a loopback database named `gcp_chat_test`; it is not
an operator tool for an existing service. Group 10 also builds/runs the offscreen settings QML test in that job.
The Windows `desktop_text_messaging`
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

Group 9 adds `desktop_screen_viewer_render_and_lifecycle` (actual Qt SceneGraph
with a protocol fixture) and `screen_viewer_frame_credit_and_scope` (portable
sanitizers). The API runner also requires
`--viewer-fixture build/chat-api/gcp-view-process-fixture`; it checks real viewer
leases/discovery and negative live-track authorization without fabricating media.
The separate Windows synthetic SFU contract uses `screen_viewer.cpp`, the same
exact-track receiver core as production. See [viewer evidence](desktop-screen-viewer.md).


Group 10 adds `desktop_preferences_and_recovery` on Windows and Linux, using
isolated temporary profiles, real files and HTTP fixtures, plus the shipped
SettingsPanel and AccountPanel. The Linux standalone CMake now requires Qt Quick
and Quick Controls as well as Core/Gui/Network/WebSockets; run
`ctest --test-dir build/chat-api --output-on-failure` after building. Actual
Qt→Rust acceptance also runs the production readiness probe and manual session
rotation. The Windows voice fixture checks preference restoration, missing and
returning devices and session/device failure cleanup without hardware capture.
See [settings and recovery evidence](desktop-settings-recovery.md).


Group 11's gaming tooling workflow runs Python analyzer/integrity tests and both
Windows PowerShell 5.1/PowerShell 7 policy and real process-only collection tests.
The latter compile bounded sleeping fake desktop/voice/game processes, never use
real audio/video/game capture, verify child-loss invalidation and pass actual
PowerShell JSONL through the Python reader. Synthetic GPU rows test attribution;
missing VM GPU data stays incomplete. A successful workflow packages only an
allowlisted set of operator source tools, guide and empty configuration, with a
commit/hash inventory and ZIP checksum. No raw captures or fixtures are bundled.
See [desktop gaming measurement](desktop-gaming-qualification.md); CI cannot
satisfy actual gaming/media acceptance or justify speculative quality tuning.
