# Streaming and encoder performance diagnostics

## Objective

Verify **CPU, GPU, RAM, and gaming frame-time impact** before choosing a media encoder architecture. The currently implemented display-to-LiveKit prototype does GPU-to-CPU readback and lets the SDK choose its internal encoder. It does not yet force NVENC, AMD AMF, Intel Quick Sync, HEVC or AV1.

## Build and inventory

Build Release configuration on Windows. The `gcp-media-probe.exe` diagnostic is independent of the optional LiveKit SDK.

```powershell
$probe = ".\\build\\desktop\\Release\\gcp-media-probe.exe"
& $probe --list-gpus
& $probe --list-encoders
& $probe --benchmark-copy --width 1920 --height 1080 --iterations 300
```

The GPU command uses DXGI, returning adapter name, vendor/device ID, dedicated/shared memory and software-adapter indicator. The encoder command lists **registered** Windows Media Foundation H.264/HEVC encoding transforms. Entries can include software transforms; registration is not a verified capability for particular resolutions, bitrates, frame rates or codecs.

The copy benchmark prints a local JSON record with process CPU estimate, final private memory, copy bandwidth and dimensions. It excludes encode, transport, GPU readback, and all game load. High throughput here does **not** imply minimal in-game impact.

## End-to-end verification

On a **real interactive Windows machine**, publish a deliberately selected 1080p monitor using the opt-in `share-monitor` command described in [monitor sharing](monitor-share.md). Observe both publisher and receiver, never just publisher offered FPS.

Record per test case:

| Measurement | Source |
|---|---|
| GPU make, model, driver | DXGI and Windows device information |
| Candidate H.264/HEVC MFTs | Diagnostic inventory |
| CPU and private memory | Process telemetry |
| Capture arrivals / rejected / dropped | Monitor-share JSON counters |
| GPU staging readback mean, p50 and p95 | Capture telemetry |
| Publisher offered frames | Monitor-share JSON |
| Viewer received frames | LiveKit watch JSON |
| Actual codec and encoder backend | WebRTC / SDK statistics and GPU video-engine counters |
| Game average FPS and 1% lows | Repeatable frame-time traces, with and without sharing |
| Network loss, bitrate, delay | SFU/WebRTC statistics |

Start with 720p30; proceed to 1080p30 and 1080p60 only after correct receiving and stable memory. If GPU hardware encoding cannot be confirmed, report it as **unknown**, not successful.

## Safety and constraints

`--list-gpus`, `--list-encoders`, and `--benchmark-copy` do not access the screen or microphone. The separate monitor-share test **does** transmit display content and requires explicit consent. Never put credentials, JWTs, or sensitive screen data into reports.

Windows hosted CI can verify compilation and the synthetic CPU copy benchmark, but cannot reproduce GPU/game performance. There are **no measured gamer-PC FPS, encoding or GPU utilization results** in this repository yet.
