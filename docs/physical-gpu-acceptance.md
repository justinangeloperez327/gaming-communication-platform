# Group 16 — Physical Windows GPU acceptance runbook

**Status:** The Group 16 scripts make physical validation reproducible; no physical hardware result has been collected by CI. A successful offline validator run is **not** a production release certificate. Never commit JWTs, raw desktop content, telemetry containing personal details, or large diagnostic logs.

## Equipment and prerequisites

Use a Windows gaming PC with an interactive desktop, a compatible D3D11/H.264 hardware encoder, and a separate second participant (ideally on another PC). Set up a dedicated LiveKit test room with short-lived, distinct publisher and viewer JWTs. Select a test-only display; screen sharing transmits **everything visible on the selected monitor**. Confirm both participants are permitted to view the test content.

Build with `GCP_ENABLE_LIVEKIT=ON`. Follow [stream validation](stream-validation.md) for the full native build commands, monitor selection and receiver invocation. There is no unattended production capture service.

## Collect evidence in a single controlled session

1. On the **viewer**, start `gcp-livekit-smoke.exe watch --sender "publisher-identity" --track h264 --seconds 180`, with the viewer token and correct room. Save its standard output as `receiver.log`.
2. On the **publisher**, start a local PowerShell window collecting the GPU video-encode engine during the same run:

   ```powershell
   .\scripts\sample-gpu-video-encode.ps1 -Output ".\gpu-video-encode.csv" -Seconds 60
   ```

3. On the **publisher**, use a separately scoped JWT and explicitly authorize the intended test monitor:

   ```powershell
   .\build\desktop\Release\gcp-livekit-hardware.exe --publish-monitor `
     --monitor 0 --confirm-screen-share yes `
     --width 1920 --height 1080 --fps 30 --duration 60 `
     --bitrate 4000000
   ```

   Save its standard output as `sender.log`. Replace dimensions with the **native** selected-display resolution. Prefer an animated test scene; Windows Graphics Capture does not necessarily emit repeated frames from static content. The GPU counter script only observes OS counters, and may not work on some drivers.

4. Visually verify the viewer receives the **correct test display**, not merely one decoded frame. Record date/time, GPU model, driver, Windows version, stream dimensions/FPS/bitrate, selected encoder identity and any decoder problems in your local test notes. Do not put room tokens in those notes.

5. Run the offline consistency checker (Python 3, standard library only):

   ```powershell
   py scripts\validate-hardware-stream.py `
     --sender sender.log --receiver receiver.log `
     --gpu-csv gpu-video-encode.csv `
     --output stream-evidence.json
   ```

The input files may include other diagnostic prose; the checker extracts exactly one JSON record with `probe=hardware_h264_livekit` and one with `probe=livekit_decoded_frame_timing`. Multiple such records are rejected to avoid silently combining distinct runs. The output is a compact, sanitized evidence summary, not a copy of input logs or credentials.

**Exit codes:** 0 = supplied transport logs show at least two decoded callbacks, positive sender packets/keyframes, and zero SDK rejections; 2 = transport observation gate failed; 1 = malformed/missing evidence. A 0 does *not* establish actual GPU acceleration, frame correctness, codec quality, same-room identity, low network latency, or an acceptable game FPS impact.

The GPU CSV only establishes observed activity of Windows `engtype_VideoEncode` performance counters. Other programs may use the engine. Attribution to the GCP process and selected physical encoder must be corroborated independently with appropriate profiling tools, along with actual bitrate/IDRs and decoder quality.

## Performance acceptance

Collect at least three matched idle/voice/share gaming runs at a defined hardware/graphics preset, with GPU engine and CPU/RAM measurements. Export frame durations in milliseconds and use [the baseline-vs-streaming frame-time comparison](../scripts/compare-frame-times.py) with explicit input column. Evaluate *both* average FPS and 1%-low FPS; the provisional 5% regression objective is not an achieved measurement.

Also verify stop/start, loss of stream, network reconnection, display resize/change, microphone privacy, and sustained 1080p60 before declaring the capability production-ready. The current diagnostic max duration is **300 seconds**, and it lacks native Qt stream controls and full production session recovery.

## What CI verifies

CI executes the Python evidence checker unit tests and builds the Windows native executables. It does not initiate a LiveKit server, launch screen capture, measure GPU engine counters, or manufacture a successful result. **Group 16 acceptance remains pending physical tests** until real device and viewer evidence is reviewed.
