# Group 42 — Windows gaming benchmark capture harness

Group 41 is merged into main. This group adds a read-only Windows PowerShell process sampler to complement the existing frame-time CSV and native LiveKit voice measurements. No actual game, headset, or competitor benchmark is executed in GitHub CI.

## Process sampling

Run on a Windows gaming PC while a representative game scene is active:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/sample-game-process.ps1 -ProcessName gcp-desktop -DurationSeconds 120 -IntervalSeconds 2 -OutputCsv runs/gcp-desktop.csv
```

Run the same command against the actual native voice executable process name (`gcp-livekit-voice`) while voice is active. Keep game FPS measurements separate with PresentMon or another validated frame-time exporter.

The sampler uses Windows `Get-Process` CPU time and private bytes, normalized by logical processor count over elapsed wall time. The results are approximate process-only resource snapshots, not GPU usage or end-to-end frame times. They can be inaccurate if multiple same-named processes restart during the window, so verify process identity manually before accepting runs. No admin rights, network calls, token access, screen capture, or audio recording are required.

For repeatable qualification: gather at least three paired baseline/GCP runs on the same game hardware, settings, level, FPS cap, thermals, and Windows power mode. Alternate test order, record hardware and software versions, capture actual frame-time exports, then run `benchmark-gaming-voice.py` and `qualify-gaming-voice.py`. Use separate matched Discord runs before making any product comparison claims.

**Not yet achieved:** physical voice checks, real CPU/GPU/RAM baselines, actual gaming captures, sustained memory profiling, and qualified acceptance thresholds. GitHub Actions compilation success alone does not establish the app's real-world resource efficiency.
