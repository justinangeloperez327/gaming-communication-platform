# Hardware H.264 → LiveKit diagnostic

This is a **developer-only test** of the real monitor capture → GPU BGRA-to-NV12 conversion → asynchronous Media Foundation hardware H.264 encoder → LiveKit encoded-video publishing path.

The `gcp-livekit-hardware.exe` binary is built only with `GCP_ENABLE_LIVEKIT=ON`. It is not integrated into the Qt user interface and is not production screen sharing.

## Privacy and prerequisites

**The command below transmits the entire selected monitor to the room.** Windows notifications, messages, passwords displayed on screen, and private application windows might be visible to authorized participants. Other room participants may record what they receive. Use a dedicated test display and test content only.

Requires Windows 10/11 x64; compatible D3D11 video processing and an asynchronous D3D11-aware hardware-class H.264 Media Foundation encoder; an interactive monitor with native even dimensions no greater than 1920×1080; a LiveKit server and **two different** short-lived JWTs with appropriate room permissions.

No microphone or system audio is captured. No H.264 packets are stored on disk by this diagnostic.

## Build

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" `
  -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

## Hardware publishing

Identify the monitor and its native dimensions first:

```powershell
.\build\desktop\Release\gcp-media-probe.exe --list-monitors
```

Then use a *publisher* JWT in a dedicated PowerShell terminal:

```powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<publisher-jwt>"
.\build\desktop\Release\gcp-livekit-hardware.exe --publish-monitor `
  --monitor 0 --confirm-screen-share yes `
  --width 1920 --height 1080 --fps 30 --frames 30 `
  --bitrate 4000000 --timeout 30
```

**The command starts sharing as soon as the LiveKit room and encoder are ready.** The command is bounded by a maximum of 60 submitted input frames and a 120-second timeout. It does not capture audio.

In another terminal, use a different *subscriber* JWT in the same room:

```powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<subscriber-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe watch `
  --sender "publisher-identity" --track h264 --seconds 30
```

For a remote endpoint, use `wss://`. Never put LiveKit JWTs or API secrets in Git or CI.

## Encoded packet validation

The encoder output is provided through a native callback. Complete H.264 access units are validated and normalized to **Annex B** (3/4-byte start codes, or 4-byte-length AVCC input). The first transmitted frame must be an IDR containing both SPS and PPS; every subsequent IDR is checked the same way. Inconsistent keyframe metadata, invalid encoded lengths, malformed NAL headers, nonmonotonic timestamps, or rejected SDK input cause the diagnostic to fail.

This strict behavior means that a vendor encoder using an unsupported H.264 output format, or placing parameter sets only in its output media type, may **fail the diagnostic**. The next encoder integration may need to extract/validate `MF_MT_MPEG_SEQUENCE_HEADER` and adapt supported profiles before accepting that encoder. Do not interpret a rejection as evidence that the GPU cannot encode H.264.

LiveKit's `EncodedVideoSource::captureFrame` copies submitted packets; this path is not zero-copy for encoded bytes. The application counts **unhandled** keyframe and rate-control feedback; real encoder reconfiguration in response to SFU feedback has not been implemented.

## Verification scope

GitHub-hosted Windows CI can compile the native bridge and run the **H.264 access-unit/CLI/SDK initialization self-tests** without a GPU or LiveKit credentials. It **cannot** validate an actual hardware encoder, monitor capture, SFU forwarding, packet decodability or playback, network latency, GPU encode-engine usage, game average FPS or 1% low FPS.

A zero exit status on physical hardware means the hardware-class Media Foundation encoder drained its output and the LiveKit SDK accepted at least one complete keyframe-bearing stream. **It does not independently prove remote playback.** The receiving client must show decoded frames, and a controlled gaming-PC benchmark must confirm resource and FPS impact.

The current diagnostic deliberately uses a finite frame budget. Production work still requires keyframe generation, bitrate adaptation, longer-lived lifecycle, capture resizing/recovery, frame pacing, buffering limits, privacy controls and native UI integration.
