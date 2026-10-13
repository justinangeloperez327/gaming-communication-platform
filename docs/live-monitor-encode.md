# Local Windows monitor → GPU H.264 diagnostic

This opt-in diagnostic is the next step beyond synthetic test frames. It acquires **actual selected monitor frames** via Windows Graphics Capture, converts them **entirely on the same D3D11 GPU device** from BGRA to NV12, and submits GPU-resident Media Foundation samples into the existing asynchronous hardware-class H.264 encoder.

**No screen content is transmitted to LiveKit, saved to disk, or exposed to the network by this executable.** Encoded video bytes are counted and discarded. Nevertheless, the diagnostic temporarily handles potentially sensitive monitor content in GPU memory. Run it only on a display you intend to test.

## Requirements

- Windows 10/11 with Graphics Capture support and an interactive desktop session.
- A hardware D3D11 GPU with BGRA/NV12 video processing and an asynchronous D3D11-aware H.264 hardware-class Media Foundation transform.
- A monitor with an even width and height no larger than 1920×1080. The requested dimensions must match its **native** size exactly; the prototype does not resize or crop.

## Run

The normal Release build includes the diagnostic even if LiveKit integration is disabled.

```powershell
# Identify physical monitor indices and native dimensions.
.\build\desktop\Release\gcp-media-probe.exe --list-monitors

# Explicitly authorize monitor 0, here assumed native 1920x1080.
.\build\desktop\Release\gcp-hardware-h264.exe --run `
  --source monitor --monitor 0 --confirm-monitor-share yes `
  --width 1920 --height 1080 --fps 30 --frames 30 `
  --bitrate 4000000 --timeout 30
```

The `--source monitor` mode refuses to proceed without **both** a valid monitor index and the literal `--confirm-monitor-share yes`. Source modes `nv12` and `bgra` remain synthetic diagnostics that do not capture the display.

The run is bounded to at most 60 encoder-submitted frames and 120 seconds. A frame arriving after the selected monitor is resized causes capture to stop. If Windows does not provide fresh display changes or the GPU converter encounters an error, the test fails instead of inventing a frame rate.

## Implementation boundaries

- **One D3D11 hardware device** supplies the Graphics Capture frame pool, D3D11 video processor and Media Foundation device manager.
- **No GPU-to-CPU pixel readback** occurs in the new conversion path. CPU control metadata and GPU driver work still incur overhead.
- An internal latest-frame slot replaces older GPU samples rather than building an unbounded capture queue.
- Each converted NV12 sample owns its texture. No output texture is reused while the asynchronous MFT may still reference it. This bounds the consumer queue, **not** the number of samples internally buffered by a driver.
- Input FPS throttling reduces unnecessary conversion work.
- Encoder output is counted, **not packetized into Annex B H.264 access units** for transport.
- No microphone, per-window privacy selection, HDR handling, cursor policy, device-reset recovery, screenshot saving, or LiveKit publishing is implemented in this mode.
- Hardware-class selection and actual GPU video-processor/encoder operation are **not verified on GitHub's hosted Windows runner**. CI only compiles and validates the argument/smoke-test paths.
- A physical gaming PC must measure encoder GPU engine use, CPU/RAM footprint, end-to-end encode latency, and game FPS/1% lows before this path is described as optimized gaming streaming.

## Next gate

Extract validated H.264 access units (including SPS/PPS, keyframe metadata and correct timestamps) from the hardware MFT, and connect them to LiveKit's existing `EncodedVideoSource`. Add feedback-aware rate/keyframe control and receiver statistics. Check actual SFU delivery and gaming impact under controlled hardware benchmarks.
