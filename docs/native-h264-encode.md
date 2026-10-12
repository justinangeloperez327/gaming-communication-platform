# Native H.264 encoding benchmark

This is an offline **real H.264 encoding** benchmark of Windows Media Foundation. It generates synthetic NV12 frames and encodes them into a temporary MP4, deleting the file when complete. No screen, microphone, camera, or network content is accessed.

## Execute on Windows

```powershell
.\build\desktop\Release\gcp-media-probe.exe --benchmark-h264 `
  --width 1280 --height 720 --fps 30 --frames 90 --bitrate 4000000
```

The test accepts width 160–1920, height 90–1080, 1–60 fps, 1–180 frames and 100 kbps–16 Mbps bitrate. Dimensions must be even. It reports encoding throughput, elapsed time, encoded MP4 bytes, CPU and private-memory samples in JSON.

The Media Foundation Sink Writer is created with `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS=TRUE`. This allows use of hardware MFTs **but does not force hardware**; Windows may choose a software encoder. The tool always reports `hardware_encoder_confirmed:false`. A CI success is evidence that H.264 video was encoded, not proof of GPU assistance.

The input is CPU-generated NV12; there are no GPU textures, no LiveKit publishing, no bitrate/keyframe feedback and no game frame-time measurements. This test is intentionally independent of the pre-encoded file streaming diagnostic.

## Hardware qualification

Only mark hardware encoding complete when a known GPU/driver and selected encoder produce validated H.264 access units with GPU video-encode engine activity under real display capture, and the remote receiver verifies delivery. Benchmark game FPS and 1% lows with and without streaming. If the hardware path is unavailable, present it as unsupported instead of silently claiming acceleration.
