# Group 39 — Low-overhead native voice telemetry

This change extends the Windows native LiveKit voice diagnostic with **two-second sampled private-memory peaks**. It also includes total-machine-normalized process CPU percentage, start/end private memory and sample count in the existing final JSON probe record. Sampling is performed in the existing session loop, not in an extra thread. Failures to obtain a sample are ignored so diagnostics cannot terminate an otherwise healthy call.

The recorded metrics are **observations**, not optimization guarantees. The peak is the maximum of periodic snapshots and start/end memory; it may miss spikes shorter than the sampling interval. CPU percent is process CPU time divided by wall duration and logical processor count, not a per-core or GPU utilization metric. A five-minute test is still diagnostic-only; it does not establish production latency, audio quality, or game FPS impact.

## Acceptance protocol

1. Run on the same Windows device with the same game and graphics settings; establish idle gaming baseline without the GCP process.
2. Run a listen-only session and an explicit-consent microphone session. Use similar map/game action for each condition.
3. Collect total-system FPS/frame-time percentiles separately alongside GCP's output; compare p50/p95/p99 frame time, CPU percent, peak memory and voice glitches.
4. Test with headset hotplug, network reconnection, and two actual participants.
5. Do not save access tokens, room tokens, microphone raw audio or speaker recordings alongside metrics.

This PR does not add GPU monitoring or assert a CPU/RAM improvement until repeated comparative measurements are available.
