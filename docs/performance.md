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
