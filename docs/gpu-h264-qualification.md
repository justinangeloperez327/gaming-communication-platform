# GPU H.264 device and Media Foundation qualification

This diagnostic exercises the **device and type-negotiation prerequisites** for GPU-resident H.264 encoding on Windows. It does not encode an actual frame, publish media, open the screen, or record audio.

## Execute on a physical gaming PC

Build the Windows project, then run:

```powershell
.\build\desktop\Release\gcp-media-probe.exe --list-gpus
.\build\desktop\Release\gcp-media-probe.exe --qualify-gpu-h264 `
  --width 1920 --height 1080 --fps 60 --bitrate 8000000
```

A successful qualification requires all of the following:

- Creating a D3D11 **hardware** video device (no WARP fallback).
- Allocating an NV12 GPU texture and wrapping it with `MFCreateDXGISurfaceBuffer` inside an `IMFSample`.
- Enumerating H.264 encoders with Media Foundation's **hardware-only** flag, not the general software/hardware list.
- Activating an encoder that advertises Direct3D 11 awareness, including negotiating an attached `IMFDXGIDeviceManager`.
- Negotiating the requested H.264 output type, NV12 input type, frame rate, resolution and bitrate, then obtaining the output-stream allocation requirements.

If **none** of the enumerated hardware-class encoders qualifies, the command exits with code 2. A failure should be investigated, not silently replaced with a software encoder. If a graphics device or Media Foundation API fails before qualification, the command exits with code 1.

A successful result says **configured and negotiated**, not **hardware-encoded**. The GPU texture in this test is unpopulated and is never submitted to `ProcessInput`. No packets are generated. An asynchronous hardware transform requires Media Foundation event-driven input/output management; constructing a synchronous encoder loop would not satisfy its contract.

This command reports the negotiated candidate and whether its output sample must be supplied by the caller or encoder. It does **not** claim selected physical GPU identity, video engine utilization, encode throughput, or end-to-end delivery.

## CI and safety

The GitHub-hosted Windows runner compiles this code and tests argument rejection and profile validation. It **does not** exercise the hardware-qualified path. The command does not obtain screen pixels or access microphones and makes no network connections.

## Subsequent implementation

The next native work is to submit initialized NV12 GPU textures to an explicitly selected hardware transform, handle `METransformNeedInput` and `METransformHaveOutput`, collect complete encoded access units, translate keyframe/bitrate feedback, and deliver through LiveKit's `EncodedVideoSource`. Only then should game-FPS, GPU encoder usage and 1080p60 performance be assessed.

See Microsoft's documentation for [hardware MFTs](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts), [D3D-aware transforms](https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-aware-mfts) and [DXGI sample surfaces](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatedxgisurfacebuffer).
