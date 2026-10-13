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


## Group 15 — bounded longer-running diagnostic sessions

The original hardware monitor publisher stopped at **60 input frames**, making sustained observations impossible. Both local-only `gcp-hardware-h264.exe` and the opt-in network publisher now accept `--duration 1..300` in **monitor mode only**. When duration is set, the maximum input-frame budget defaults to `fps × duration` (capped at 18,000 frames). An explicit lower `--frames` limit can end the run sooner. The output/drain deadline defaults to `duration + 15 seconds`; a custom `--timeout` must allow at least 5 seconds beyond the requested duration. No stream may remain active beyond 330 seconds including drain. With no `--duration`, the original short frame-count test behavior is retained.

For a **local-only** 2-minute diagnostic:

```powershell
.\build\desktop\Release\gcp-hardware-h264.exe --run `
  --source monitor --monitor 0 --confirm-monitor-share yes `
  --width 1920 --height 1080 --fps 30 --duration 120
```

For a 2-minute **LiveKit publisher** (transmits the entire chosen display):

```powershell
.\build\desktop\Release\gcp-livekit-hardware.exe --publish-monitor `
  --monitor 0 --confirm-screen-share yes `
  --width 1920 --height 1080 --fps 30 --duration 120
```

Start the second participant's receiver for longer than the sender, for example `watch --sender "publisher-identity" --track h264 --seconds 150`. Use dedicated test content and separate short-lived JWTs. The application has no production permission control or capture picker.

When Windows Graphics Capture delivers no fresh frames (e.g. a static desktop), the encoder waits for an updated frame until its bounded deadline rather than exiting after a 2-second no-change period. The diagnostic reports `monitor_idle_waits`, `submitted_input_samples` and `time_limit_reached`. There is **no fabricated or repeated video frame**. This may reduce observed FPS during a static scene and is not equivalent to a constant-bitrate 30/60 FPS screen stream.

**Physical acceptance remains mandatory:** run at least three paired game workloads, validate that the remote receiver decoded real frames, inspect GPU Video Encode engine activity, and compare CPU/private memory and game frame-time results. The asynchronous hardware MFT may retain its own surfaces internally; no memory-stability guarantee can be inferred from the bounded latest-frame capture slot. A 300-second cap is a diagnostic safety bound, **not proof of stable long-term sessions or optimized 1080p60**.

Hosted CI only exercises parsing, consent, build and self-tests. It does not start WGC, GPU hardware encoding, a LiveKit server or actual gameplay.
