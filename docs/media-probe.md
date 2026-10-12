# Windows native media feasibility probe

This diagnostic is separate from the Qt desktop UI. It measures operating-system capture behavior rather than representing an implemented screen-sharing or voice-calling product.

## Prerequisites

- Windows 10 version 1903 (build 18362) or later for Windows Graphics Capture.
- Compatible D3D11 graphics device and an interactive local desktop session.
- Windows microphone permission for command-line/desktop applications if testing audio capture.
- CMake, Visual Studio 2022 C++ workload and Qt as documented in [development](development.md).

After building, locate `gcp-media-probe.exe` under `build/desktop/Release/`.

## Commands

```powershell
$probe = ".\build\desktop\native\probe\Release\gcp-media-probe.exe"
& $probe --help
& $probe --list-monitors
& $probe --list-audio
& $probe --capture-monitor 0 --seconds 10
& $probe --probe-mic --seconds 10
```

To save **metrics only** (no video/audio payload):

```powershell
& $probe --capture-monitor 0 --seconds 30 > capture-metrics.json
```

`--capture-monitor` enumerates monitor handles, creates a GPU-backed Windows Graphics Capture frame pool, counts received frames and releases each frame. It does not encode, persist, or transmit video. It does not explicitly copy GPU frames to the CPU.

`--probe-mic` opens the default communications microphone through event-driven shared-mode WASAPI, counts incoming audio frames and releases each audio packet immediately. It does not read, log or save microphone samples.

Both commands emit one JSON object with elapsed time, frame count, callback error count (under `packets` for monitor capture), process CPU percentage across all logical processors, client private bytes at start and end, and frame dimensions where applicable.

## Important limitations

- **Observed FPS is not stream FPS.** Static desktops can produce fewer frames, and no encoder is present.
- **CPU percentage is a coarse point-to-point measurement.** It does not include the game or system-wide GPU cost, and it must be tested over representative sessions.
- **No microphone quality verification.** Packet arrival does not demonstrate good echo cancellation, latency, or audio intelligibility.
- **No audio/video streaming.** No LiveKit/WebRTC implementation, codec negotiation, network congestion control, or remote client is wired in.
- **No automated GPU performance testing.** GitHub-hosted runners are useful for compile/smoke checks but generally cannot validate interactive GPU capture. Benchmark on physical gaming PCs.
- **No capture permissions UX.** Use only deliberately selected monitors. Sensitive information on a chosen monitor may be present in capture buffers even though the probe never stores it.
- The probe is not a production capture engine: resizing, DXGI device loss, HDR conversion, overlays, foreground behavior and window capture all require further implementation and tests.

## Next feasibility gate

1. Prove 1080p60 end-to-end encoder capability (H.264 hardware path).
2. Integrate supported LiveKit C++ SDK as a pinned optional dependency, obtain scoped server-issued test tokens.
3. Publish a test video track, view it from another client, and measure latency, dropped frames and game frame times.
4. Implement audio playback and end-to-end voice test, including reconnects and hot-swapped devices.
5. Document measured GPU encoder utilization, system load, and the 1% low game FPS impact before designing production streaming behavior.

## External references

- Microsoft Windows Graphics Capture `CreateForMonitor`: https://learn.microsoft.com/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor
- WinRT `Direct3D11CaptureFramePool::CreateFreeThreaded`: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded
- WASAPI event-driven capture: https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturesharedeventdriven
- LiveKit Windows C++ SDK: https://docs.livekit.io/transport/sdk-platforms/cpp/
