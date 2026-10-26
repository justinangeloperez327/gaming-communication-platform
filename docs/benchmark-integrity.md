# Group 44 — Benchmark integrity and end-to-end tests

Group 43's CI and benchmark tooling checks passed. This change tightens capture integrity before interpreting gaming performance results.

The benchmark analyzer rejects the same pathname reused as a baseline, GCP frame-time, or voice telemetry input, and rejects a zero-duration voice session. These checks reduce the risk of an accidentally invalid comparison being accepted as evidence; they cannot verify that the captures were made on physical gaming hardware, or that labels correspond to genuinely independent runs.

The test suite now exercises the whole report pipeline with **synthetic fixture data**, verifying successful qualification, regression failure (exit code 2), artifact generation, and insufficient-run handling. Synthetic test data is not benchmark evidence.

Run checks with:
```powershell
python -m unittest discover -s scripts -p "test_*.py" -v
python -m compileall -q scripts
```

Still needed for a real performance claim: three or more properly matched Windows gameplay captures per condition, system specifications, audio quality and GPU measurements, and matched competitor trials. Do not represent CI test success as measured in-game performance.

Group 11 additionally rejects path aliases/hard links and prevents overwrite of
prior pipeline results. Its [desktop analyzer](desktop-gaming-qualification.md)
binds input hashes, actual process identities and one QPC time window, validates
build/boot/duration/order, and gates every run rather than accepting a median
that hides a regression. Synthetic or operator-declared origin is never
authenticated as physical evidence.
