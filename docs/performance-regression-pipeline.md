# Group 43 — Automated performance reporting

The existing Python benchmark and qualification tools can now be executed as a single pipeline using `scripts/run-gaming-qualification.py`. It consumes **real captured** frame-time CSV files and native voice diagnostic logs, writes `benchmark.json`, `qualification.json` and a human-readable `summary.md`, and propagates regression failures as exit code 2. Invalid or missing captures fail as exit code 1.

Example (replace these filenames with captures from a real Windows system):

```powershell
python scripts/run-gaming-qualification.py --baseline runs/b1.csv runs/b2.csv runs/b3.csv --gcp runs/g1.csv runs/g2.csv runs/g3.csv --voice-json runs/v1.txt runs/v2.txt runs/v3.txt --column MsBetweenPresents --warmup-seconds 30 --max-fps-loss-pct 5 --max-cpu-pct 2 --max-peak-mib 200 --max-p99-increase-pct 10 --output-dir runs/qualified
```

The numbers above are **illustrative operator-supplied limits**, not established release requirements. At least three paired runs are mandatory.

The GitHub Actions `gaming-benchmarks.yml` workflow checks Python syntax and runs analyzer unit tests. It **cannot collect physical Windows gaming data** on hosted CI, cannot measure Discord, and does not certify acceptable CPU/GPU/RAM usage. Actual benchmarking and physical audio acceptance remain separate release requirements. Never commit tokens, raw audio, or proprietary game capture data.
