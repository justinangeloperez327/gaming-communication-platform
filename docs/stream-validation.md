# Group 14 — Real-device streaming and game performance validation

The GCP native media code now instruments encoder output, accepted LiveKit packet submission, and **decoded receiver callbacks**. No hosted CI service can prove these observations on a physical GPU. Run actual tests on an interactive Windows gaming PC before claiming efficient screen/game streaming.

## What each observable measures

| Observation | Source | Important exclusion |
|---|---|---|
| Encoded MFT samples/bytes, time to first encoded output, output inter-arrival p50/p95 | `gcp-hardware-h264` JSON | Not per-frame encoder latency; async output may be reordered |
| Accepted H.264 payload bytes, packet-arrival p50/p95, accepted first-frame time | `gcp-livekit-hardware` JSON | SDK acceptance is not proof of SFU forwarding or playback |
| Publisher CPU estimate and private memory | Windows process snapshots | Total-machine normalized process CPU, not GPU use |
| Decoded callback count, time to first callback and inter-arrival p50/p95 | `gcp-livekit-smoke watch` JSON | Does not measure network end-to-end latency or video quality |
| Average game FPS and 1%-low FPS | Exported game frame-time CSV comparison | Requires separate real gaming workload |
| GPU Copy/Video Encode/3D engine load | Windows GPU performance tools / driver telemetry | Not collected automatically by the GCP diagnostic |

**Metrics are separate JSON records**, not an automatically synchronized sender/receiver trace. Frame-arrival p95 describes callback timing irregularity; it is **not encoder time, network jitter or latency**.

## Run a two-participant test (manual)

Use a Windows computer with a supported hardware H.264 MFT, a deliberately chosen display of even native dimensions up to 1920×1080, a LiveKit test room and **two distinct short-lived JWTs**. The hardware publisher transmits **everything visible** on that monitor; use only test content.

First start a receiver in a separate terminal (or on another device) and keep it running longer than the publisher:

```powershell
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<viewer-jwt>"
.\build\desktop\Release\gcp-livekit-smoke.exe watch `
  --sender "publisher-identity" --track h264 --seconds 30
```

Then on the publishing Windows PC, with a **different** participant token and a confirmed monitor selection:

```powershell
.\build\desktop\Release\gcp-media-probe.exe --list-monitors
$env:LIVEKIT_URL = "ws://127.0.0.1:7880"
$env:LIVEKIT_TOKEN = "<publisher-jwt>"
.\build\desktop\Release\gcp-livekit-hardware.exe --publish-monitor `
  --monitor 0 --confirm-screen-share yes `
  --width 1920 --height 1080 --fps 30 --frames 60 `
  --bitrate 4000000 --timeout 30 --linger-ms 1000
```

The receiver should report **decoded frames greater than zero**. The publisher should report **accepted H.264 packets greater than zero** with a keyframe. A publisher-only result does **not** pass this gate. Record OS/driver/GPU model, effective codec profile and actual GPU video-engine activity as separate observations. Never commit room tokens or test video.

The diagnostic is **finite and short** (at most 60 encoder-input frames) and remains developer-only. This test cannot prove sustained streaming or stable gaming FPS under a long session.

## Compare real game-frame times

Capture **paired, representative gaming workloads** using a frame-time recorder such as PresentMon or a trusted equivalent. Export CSV with a numeric per-frame duration column in **milliseconds** (not sampled FPS). Keep resolution, graphics settings, scene, thermal conditions and capture method constant. Collect at least three baseline and three streaming runs; exclude warmup consistently.

```powershell
py scripts\compare-frame-times.py `
  --baseline baseline1.csv baseline2.csv baseline3.csv `
  --streaming sharing1.csv sharing2.csv sharing3.csv `
  --column MsBetweenPresents --warmup-seconds 10 `
  --output fps-comparison.json
```

**Use the exact exported frame-time column name**; column labels vary by recording tool/version. The tool is standard-library Python with no extra dependencies. It requires at least 100 valid frame intervals *per run after warmup*, rejects blank/nonpositive/NaN values, and compares per-run median results across the two conditions.

- **Average FPS** = 1,000 ÷ mean frame time (ms).
- **1%-low FPS** = 1,000 ÷ mean of the slowest 1% of frame durations. This definition must be retained across both sets.
- **p99 frame time** = nearest-rank 99th percentile of frame durations.
- **Change (%)** = (streaming FPS / baseline FPS − 1) × 100. A negative value indicates a regression.

An *optional engineering threshold* may be configured with `--max-regression-pct 5`: the command exits 2 if **either** average or 1%-low FPS regresses by more than 5%. This is a configurable target, **not a measured result or an established release guarantee**.

## CI and release gates

CI validates compilation, deterministic frame-timing self-tests, and the pure-Python comparison calculations. **CI does not execute an actual gaming GPU or two-client session.** Tests do not collect screenshots, audio, game telemetry, driver utilization or network traffic.

Before declaring production hardware streaming, demonstrate: decoded receiver frames, consistent keyframes and codec negotiation, stable memory, GPU engine utilization, bitrate feedback effectiveness, representative 1080p60 runs over minutes rather than 60 frames, and measured game-FPS impact with baseline comparisons. Only then decide if optimizations meet the product's acceptance criteria.
