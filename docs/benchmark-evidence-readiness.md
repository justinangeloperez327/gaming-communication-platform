# Group 45 — Performance evidence and release-readiness gates

Group 44's main CI and benchmark tooling tests passed. This group adds `scripts/validate-benchmark-evidence.py`, which checks a locally supplied human-authored capture manifest: Windows/game/hardware/tool metadata, at least three paired runs, distinct existing baseline/GCP/voice data files, and bounded nonempty file sizes. The validator checks metadata presence, **not truth or origin**, and never calls a synthetic fixture real benchmark evidence.

A manifest can be placed next to the local capture files:
```json
{
  "hardware_cpu": "record CPU model",
  "hardware_gpu": "record GPU model",
  "ram_gib": 32,
  "windows_build": "record version",
  "gpu_driver": "record driver",
  "game": "record title",
  "game_version": "record version",
  "graphics_preset": "record settings",
  "resolution": "2560x1600",
  "frame_capture_tool": "PresentMon",
  "frame_capture_version": "record version",
  "voice_mode": "explicit_microphone",
  "operator": "record operator identifier",
  "capture_date_utc": "2026-10-15T08:00:00Z",
  "runs": [
    {"baseline_csv": "b1.csv", "gcp_csv": "g1.csv", "voice_log": "v1.txt"},
    {"baseline_csv": "b2.csv", "gcp_csv": "g2.csv", "voice_log": "v2.txt"},
    {"baseline_csv": "b3.csv", "gcp_csv": "g3.csv", "voice_log": "v3.txt"}
  ]
}
```
Run `python scripts/validate-benchmark-evidence.py --manifest runs/manifest.json --output runs/evidence-check.json` before the existing benchmark and qualification scripts.

## Release readiness

- [ ] Windows desktop and native voice CI green on the intended release commit
- [ ] Real two-participant Windows voice and headset hotplug tests signed off
- [ ] Credential expiration, logout, channel switch and SFU participant revocation validated
- [ ] Three or more real paired gameplay captures per condition with complete metadata
- [ ] Approved FPS/1%-low/p99 frame-time, CPU, RAM and GPU utilization criteria validated on representative gaming hardware
- [ ] Matched Discord comparisons where superiority claims will be made
- [ ] Microphone authorization and fail-closed recovery privacy reviewed
- [ ] Installer, packaging, signing and release rollback procedures proven

Do not treat this checklist or validator as release certification: none of these physical acceptance gates can be closed by GitHub-hosted synthetic CI tests alone. No tokens or raw microphone recordings should enter benchmark evidence.
