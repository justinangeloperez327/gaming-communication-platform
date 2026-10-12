# Development

## Windows desktop

Install Visual Studio 2022 with Desktop development with C++, Qt 6 (MSVC 2022 x64, Qt Quick Controls), Windows SDK and CMake 3.24 or newer.

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

GitHub Actions runs for pull requests and manual dispatch only. CI compiles the Windows media probe and tests CLI primitives but does **not** execute actual microphone/monitor capture on GitHub-hosted runners. No automatic deployment, packaging, release upload, or production database changes occur on push.

Maintain isolated feature branches. Build and test locally before merging. Keep documentation about existing capabilities accurate; planned functionality belongs in issues/PR descriptions rather than pretending to exist in the README.
