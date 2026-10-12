# Hardware H.264 and pre-encoded LiveKit transport

Two independent diagnostics prepare the eventual native hardware encoder:

1. **Windows hardware-class MFT activation:** Enumerates hardware-class H.264 Media Foundation transforms and attempts activation. No frames are encoded; no input/output types or hardware throughput are verified.
2. **LiveKit H.264 access-unit ingest:** Publishes pre-encoded Annex B test-file frames through the SDK's `EncodedVideoSource`. The file was encoded **outside** this application, and this diagnostic does not select or invoke NVENC, AMD AMF, or Intel Quick Sync.

These features do **not** yet constitute GPU-accelerated live game streaming.

## Build

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" `
  -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

## Hardware encoder activation diagnostic

```powershell
.\build\desktop\Release\gcp-media-probe.exe --probe-hardware-h264
```

The command requests H.264 encoder MFTs with `MFT_ENUM_FLAG_HARDWARE`, attempts activation, and reports their status. Empty results do not rule out vendor-specific APIs. Even successful activation is not proof of working 1080p60 hardware encoding.

## Pre-encoded H.264 file sharing

The file must be Annex B H.264 with access-unit delimiter (AUD, NAL type 9) for each frame, and SPS/PPS in every IDR access unit. The first access unit must be an IDR frame. No general-purpose H.264 parsing or conversion is attempted. The user-provided width, height and FPS must match the encoded file.

Start a development LiveKit server, then create *distinct*, short-lived publisher and viewer JWTs for one room. Do not commit tokens.

```powershell
# Sender: this command SHARES the chosen encoded file with room participants.
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<sender-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe publish-h264 `
  --file "C:\videos\test.h264" --confirm-video-file-share `
  --width 1280 --height 720 --fps 30 --seconds 15
```

In another terminal using a different viewer token:

```powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<viewer-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe watch `
  --sender "sender-identity" --track h264 --seconds 15
```

Only share test footage you are authorized to distribute. A receiving participant can record it. No camera, microphone, or desktop capture is activated by `publish-h264`.

## Boundaries and reporting

- Test-file input at most 64 MiB, up to 3,600 frames, and 4 MiB per access unit.
- Explicit confirmation required. Network transmission uses the previously configured LiveKit server and room token.
- The pre-encoded publisher uses a fixed file, so it cannot generate an IDR on demand or change bitrate in response to SFU feedback; unfulfilled keyframe requests are counted.
- The SDK copies each encoded access unit as part of the native API call.
- Publisher acceptance counts **do not prove viewer playback**. Check receiver decoded-frame counts separately.
- Windows CI checks parsing, SDK linking and command-line validation only. It cannot verify H.264 playback, hardware encoding, GPU utilization or game FPS.
- Separate hardware encoder activation does not mean the externally provided H.264 file was produced by that device.

Next: create a GPU encoder integration with NV12 input, real encoded H.264 output, keyframe/rate control feedback, and on-device frame-time measurements. Validate actual sender/receiver delivery before claiming 1080p60 gaming performance.
