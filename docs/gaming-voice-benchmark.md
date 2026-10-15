# Group 40 — Reproducible gaming + voice benchmark

Use the existing frame-time CSV exporter and Group 39 native voice JSON telemetry to compare gaming performance while GCP voice is active. The standard-library-only script `scripts/benchmark-gaming-voice.py` requires paired exported frame-time CSVs for an idle/no-voice baseline and GCP voice, plus one native voice diagnostic log per GCP run.

Example (three paired runs; replace filenames with real captures):

```powershell
python scripts/benchmark-gaming-voice.py --baseline runs/base1.csv runs/base2.csv runs/base3.csv --gcp runs/gcp1.csv runs/gcp2.csv runs/gcp3.csv --voice-json runs/voice1.txt runs/voice2.txt runs/voice3.txt --column MsBetweenPresents --warmup-seconds 30 --output runs/report.json
```

The report includes median FPS, median one-percent-low FPS, p99 frame time, relative FPS changes, and median process CPU percentage and sampled private-memory peak. Each native diagnostic must contain exactly one `livekit_voice_session` JSON record with finite nonnegative CPU, positive memory/duration and at least one periodic memory sample. Generic process metrics use the distinct `livekit_voice_process_summary` name. Earlier builds emitted duplicate final-record names and never sampled periodically; recapture those logs with the corrected build. Do not use arbitrary synthetic inputs for publication-grade results.

**Method:** On the same Windows PC, game, scene, graphics settings and power mode, alternate condition order (baseline/GCP/GCP/baseline) and collect at least three valid comparable runs. Save exact CPU/GPU/RAM hardware, driver, OS, screen mode, capture tool version and duration with the report. Use a separate matched run for third-party communication apps rather than asserting comparative superiority without data. CPU is total-machine normalized *process* CPU, not total game/system CPU; sampled memory peaks may miss brief spikes. No actual GPU utilization or audible voice result is measured by this script.

No secrets or raw audio should be included in logs. Frame-time CSVs and diagnostic records are collected externally on real Windows hardware; GitHub CI unit tests validate the analyzer only and cannot qualify voice performance.
