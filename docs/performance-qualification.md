# Group 41 — Measured performance qualification

Group 40's Windows CI run failed on a malformed string literal in `native/transport/src/voice_main.cpp`; this branch includes the compilation fix.

The new `scripts/qualify-gaming-voice.py` consumes a **real** Group 40 JSON comparison report and evaluates independently specified maximum regressions for average FPS, 1%-low FPS, p99 frame time, normalized GCP voice process CPU percentage and sampled peak private memory.

Example, with **illustrative limits only** (not established production targets):

```powershell
python scripts/qualify-gaming-voice.py --report runs/report.json --max-fps-loss-pct 5 --max-cpu-pct 2 --max-peak-mib 200 --max-p99-increase-pct 10 --output runs/qualification.json
```

Exit code 0 means measured limits passed, 2 means measured regression, 1 means invalid inputs. Three paired runs minimum. Thresholds must be approved from actual representative hardware measurements before treating the result as a release gate. The report explicitly does not certify physical voice, GPU overhead or superiority over Discord.

Run `python -m unittest discover -s scripts -p 'test_*.py'` for offline analysis tests. A separate Windows native Qt/LiveKit build must pass before merging this branch.
