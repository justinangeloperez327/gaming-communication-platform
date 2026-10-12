# Monitor-to-LiveKit feasibility prototype

This is a **developer-only** test of actual Windows display capture and publishing to a LiveKit room. It does not implement production screen sharing.

## Build

Use the pinned optional LiveKit C++ SDK v1.12.2 on Windows x64:

\`\`\`powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 \`
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \`
  -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
\`\`\`

## Start a session

On an interactive Windows desktop, identify monitor indices:

\`\`\`powershell
.\build\desktop\Release\gcp-media-probe.exe --list-monitors
\`\`\`

Start a local development LiveKit server, and create two different **short-lived** room tokens with publish/subscribe grants. Never commit tokens. In the sender's terminal:

\`\`\`powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<sender-token>"
.\build\desktop\Release\gcp-livekit-smoke.exe share-monitor --monitor 0 --confirm-screen-share --fps 30 --seconds 30
\`\`\`

**This transmits everything visible on monitor 0 to authorized room participants**. Windows notifications and private content on that display can be exposed. A remote viewer could record the stream. Do not share an unintended monitor. Sharing is explicitly opted into and stops after the requested duration (maximum 300 seconds).

In another terminal with a distinct viewer room token:

\`\`\`powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<viewer-token>"
.\build\desktop\Release\gcp-livekit-smoke.exe watch --sender "sender-identity" --track screen --seconds 30
\`\`\`

## Technical characteristics

- Windows Graphics Capture + Direct3D11 reads BGRA display frames from the selected monitor.
- A GPU-to-CPU staging texture readback feeds the SDK via CPU-backed BGRA VideoFrames. At least two copies occur on the CPU: this is **not** zero-copy.
- A **single latest-frame slot** bounds queued memory. Capture may discard changed frames while the publisher catches up.
- Monitors up to 1920×1080 are supported, no scaling, HDR, resizing, game-window capture, or device-loss recovery yet.
- Frame rate is capped at 60 fps; no audio is captured.
- The test logs only telemetry, not screen images. It cannot prevent receiving participants from capturing shared content.
- The LiveKit SDK determines its actual encoding path; **hardware encoding is not verified or guaranteed**.
- GitHub Actions checks compilation and local smoke-test behavior, not real monitor access, remote connectivity or game FPS impact.

The on-device acceptance test must measure capture readback time, process CPU/private memory, GPU encoder activity, actual delivered FPS, network latency, and the game's 1% low FPS.
