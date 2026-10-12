# Performance acceptance criteria

Performance is a release constraint. These are **provisional targets**, not measurements or guarantees. Record machine configuration, Windows version, sample size, observed CPU/memory/GPU counters, game frame times, media codec and bitrate for every result.

## Reference targets

| Workload | Draft acceptance target |
|---|---|
| Idle, minimized | Less than 1% total CPU; under 150 MB client process-tree private memory; no continuous rendering |
| Text chat | Less than 3% average total CPU; under 200 MB private memory |
| Voice call | Less than 5% average total CPU; under 250 MB private memory |
| 1080p60 sharing | Under 450 MB private memory where hardware-supported; prioritize game frame times over encoder utilization |
| 1080p60 watching | Under 450 MB private memory where hardware decoding is supported |
| Game impact | No more than 5% median FPS regression as an initial objective; also record 1% lows |

These targets require tuning and may be revised only with documented data. CPU and memory budgets are conditional on specified reference hardware.

## Measurement rules

- Define the complete client process tree, not just the main executable.
- Record idle, voice-only, streaming, viewing and minimized use cases.
- Track CPU, memory, GPU 3D, encoder/decoder engines, capture frame copies, dropped frames, latency, bandwidth, and game 1% lows.
- Exercise device disconnects, network interruptions, display changes and long-running calls.
- Keep capture queues bounded, pause unnecessary offscreen decoding/rendering, and release media resources when a stream closes.
- Prefer native hardware encoders/decoders when compatible; expose a clear fallback rather than silently overloading the CPU.

There are no published benchmark results yet. Do not describe these figures as achieved until actual measurements are committed.

## Native diagnostic measurements

On an interactive Windows gaming PC, build the Windows probe in Release mode and record the following commands before testing real streaming:

```powershell
$probe = ".\\build\\desktop\\Release\\gcp-media-probe.exe"
& $probe --list-gpus
& $probe --list-encoders
& $probe --benchmark-copy --width 1920 --height 1080 --iterations 300
& $probe --list-monitors
```

- **GPU list** is a DXGI adapter inventory, showing vendor/device identifiers and reported video/shared memory. It does not identify the LiveKit encoder.
- **Encoder list** is a Windows Media Foundation registration inventory for H.264/HEVC. The results mix software and hardware candidates, and are not proof that hardware encoding is active or that an encode mode works with LiveKit.
- **Copy benchmark** measures memory-to-memory BGRA transfer throughput within this process. It **does not** measure GPU readback, WebRTC transport, graphics-core load or actual streaming frame rates.
- **Capture telemetry** now reports sampled readback p50/p95 latency, mean readback time, and a bounded dropped/replaced-frame count. The latest frame is **moved by buffer swap** to the publisher, eliminating one previous full CPU copy.

During a real test run, pair this information with Windows Performance Monitor / GPU Engine counters (3D, Copy, Video Encode), game frame-time traces, and receiver-side observed frame rate. Compare all metrics to the *same game with voice and sharing disabled*. At least three repeated runs per preset are preferable.

**Release gate for hardware encode:** Capture proof + negotiated codec + driver/GPU encode engine activity + publisher/receiver frame timing + no unacceptable game 1%-low regression. Until these are documented, do not label the media path hardware-accelerated.

Hardware-encoder integration and GPU-resident YUV conversion are **not included in this increment**.

Hardware MFT activation and pre-encoded H.264 file publishing do not establish actual GPU encoder usage or game FPS impact. See [H.264 integration](h264-integration.md).
