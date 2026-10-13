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

## Group 10 GPU-side BGRA-to-NV12 conversion

The hardware-encoder executable additionally accepts `--source bgra`. It generates synthetic SDR BGRA frames on the CPU, uploads them to the same D3D11 hardware device used by the encoder, converts BGRA to NV12 through an `ID3D11VideoProcessor`, and wraps the resulting GPU-resident NV12 texture in a Media Foundation sample. The encoder then consumes this NV12 sample through the existing asynchronous input path.

```powershell
.\build\desktop\Release\gcp-hardware-h264.exe --run --source bgra `
  --width 1280 --height 720 --fps 30 --frames 30 --bitrate 4000000 --timeout 30
```

The default `--source nv12` retains the earlier CPU-generated NV12 upload path. The new BGRA mode is fail-closed: if the actual driver does not support BGRA video-processor input, NV12 output, or the chosen hardware encoder, it reports an error rather than silently switching to CPU conversion.

The conversion code uses `ID3D11VideoProcessorEnumerator::CheckVideoProcessorFormat` to require supported formats and `VideoProcessorBlt` to perform GPU-side conversion. The output texture is allocated per in-flight sample and is not reused while the asynchronous encoder may retain it. This avoids unsafe texture overwrite but increases peak GPU allocation during a test. The encoder diagnostic is limited to 60 frames and remains for feasibility testing.

**What this establishes only when tested on a physical GPU:** whether a compatible driver can process synthetic BGRA textures into NV12 GPU surfaces and provide them to an asynchronous hardware-class H.264 encoder.

**Not included:** Windows Graphics Capture wiring, game/window capture, actual encoder backend/GPU-engine telemetry, LiveKit publication, HDR and colorimetry verification, frame queue/pool optimization, or sustained 1080p60 validation. The program still creates the test input pixels on the CPU before uploading them; the *conversion* itself avoids CPU readback.

GitHub CI compiles and runs the CLI self-test, including `--source` validation. It does not execute the GPU-dependent `--run` command. No hardware-encoder performance results can be inferred from a passing CI run.
