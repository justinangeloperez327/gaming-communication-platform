# LiveKit native transport feasibility test

The optional `gcp-livekit-smoke.exe` is a **standalone test executable**, not voice chat or game streaming. It connects to a LiveKit room and publishes or subscribes to a synthetic RGBA video track. It never captures a screen or microphone.

## Supported version and supply-chain behavior

The integration is pinned to official LiveKit C++ SDK **v1.12.2** with a fixed SHA-256 of the official Windows x64 release archive. The normal CMake build leaves it **disabled**. Its release is downloaded only when explicitly requested; it is never checked into this repository.

The upstream SDK comes under its own license and runtime distribution obligations. See the [LiveKit C++ SDK](https://github.com/livekit/client-sdk-cpp) and complete a license inventory before publishing.

## Build (Windows x64)

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" `
  -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON

cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

For an offline SDK install, use `-DGCP_LIVEKIT_SDK_PREFIX="C:/path/to/sdk"` instead of download mode. The supplied prefix must contain `lib/cmake/LiveKit`.

The diagnostic is located in `build/desktop/Release/gcp-livekit-smoke.exe`. It needs its LiveKit SDK runtime DLLs beside it.

## Local media server and tokens

A development LiveKit server is required. One option is to install and run `livekit-server --dev` (loopback/test environment only). Use the LiveKit CLI to create separate short-lived tokens for sender and receiver in the same test room with the required join and publish/subscribe grants. Do **not** commit tokens or SDK API secrets to Git.

Example for PowerShell with separate terminal sessions:

```powershell
# Terminal A — sender (identity selected when creating sender token)
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<sender-room-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe publish --width 640 --height 360 --fps 30 --seconds 30

# Terminal B — receiver, with a DIFFERENT participant token
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<receiver-room-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe watch --sender "sender-identity" --seconds 30
```

Start receiving while publishing is active. For remote LiveKit servers, use `wss://`.

## Result interpretation

Both modes print one JSON metrics line: elapsed seconds, offered or received frames, observed FPS, process CPU percentage across all logical processors, start/end private bytes, and observed dimensions. On reception, the frame counter reflects decoded frames delivered to the callback. Results are not automatically saved.

Success means the SDK connected, the synthetic video track was published, and a receiver observed decoded frames.

**Not verified by this test:** actual game/screen capture, Windows Graphics Capture to LiveKit integration, microphone transmission, codec selection, hardware encoder utilization, stream latency, frame jitter, SFU scalability, or game FPS impact.

This probe uses **CPU-filled RGBA frames** by design. Its CPU load is not representative of a future GPU-resident screen-capture pipeline. Before production streaming, we must test codec negotiation and hardware encoding on real Windows GPUs, and measure total game impact.

CI executes the help and SDK initialization self-tests without connecting to an external server. A real room connection must be tested manually on a gaming PC.
