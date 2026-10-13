# Hardware H.264 asynchronous encoder smoke test

The `gcp-hardware-h264.exe` command attempts actual hardware-class H.264 encoding from **synthetic D3D11 NV12 textures**. It deliberately does not capture the display, microphone or camera; connect to LiveKit; or write encoded packets to disk. It is a bounded diagnostic, not production game streaming.

## Windows / GPU requirements

Windows 10/11 x64, D3D11 hardware graphics device, and a Media Foundation hardware-class H.264 encoder that advertises D3D11 awareness and asynchronous operation. The test rejects WARP and does not intentionally fall back to a software transform. Not every GPU/driver exposes a supported hardware MFT.

Build the standard CMake project (Qt is required by the root project, but the new diagnostic does **not** require LiveKit).

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
.\build\desktop\Release\gcp-hardware-h264.exe --run --width 1280 --height 720 --fps 30 --frames 30 --bitrate 4000000 --timeout 30
```

Use `--help` or `--self-test` to exercise the safe CLI without a GPU. Resolution is limited to 160–1920 by 90–1080 (even values), 1–60 fps, up to 60 frames and 120 seconds. Frame data is **synthetically generated on the CPU, uploaded to NV12 GPU textures and submitted as DXGI-backed Media Foundation samples**. This is not zero-copy from the game.

The encoder has to support the selected NV12/H.264 formats and input device manager. For asynchronous MFTs, the implementation uses `BeginGetEvent`/`EndGetEvent`, processes `METransformNeedInput` and `METransformHaveOutput` in response to readiness, requests a drain after the last frame, and uses a bounded event queue with deadline. It does not call `ProcessInput` merely because a timer fired.

## Interpretation

A zero exit code means that at least one hardware-class, D3D11-aware asynchronous Media Foundation encoder produced nonempty H.264 output samples and finished its drain. A nonzero exit code means the selected profile could not be validated or a GPU/driver capability failed.

This is stronger evidence than the previous **MFT activation/negotiation** probe. However, it **does not** independently measure GPU Video Encode engine activity, validate full H.264 Annex B packet framing, prove sustained 1080p60 throughput, or demonstrate LiveKit connectivity. A vendor's driver may have quirks, and the physical test must record GPU engine counters.

**The new test must be run on an interactive physical Windows gaming PC. GitHub-hosted Windows CI only checks compilation and CLI regression tests**, not hardware encoding. Do not treat green CI as proof of GPU encoding capability.

## Remaining streaming integration

- GPU-side BGRA → NV12 conversion from Windows Graphics Capture textures, without staging CPU readback.
- Encoder lifecycle, rate and keyframe control driven by LiveKit `EncodedVideoSource` feedback, with valid H.264 RTP-compatible parameter sets/access units.
- Bounded, synchronized video delivery; actual receiver playback; drop and bitrate metrics.
- Resource profiling: CPU/GPU/RAM, game average and 1% low frame rates under realistic workload.

Sources: [Microsoft asynchronous MFTs](https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts), [hardware MFTs](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts).
