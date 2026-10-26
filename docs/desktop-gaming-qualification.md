# Desktop gaming measurement — Group 11

[PR #89](https://github.com/justinangeloperez327/gaming-communication-platform/pull/89)
is stacked on [Group 10 / #88](https://github.com/justinangeloperez327/gaming-communication-platform/pull/88).
The measurement tools and integrity checks are implemented. **Actual gaming
qualification remains pending owner captures and review.** No game, physical
headset or two-PC capture/viewing run was performed for this change. No CPU/GPU/RAM
saving or media-quality improvement is claimed, and no media settings are tuned.

## Prepare the actual Windows run

Use one tested portable/installed build on the gaming PC and the shared host.
Start the game and GCP normally. Join voice, confirm the microphone/source/viewer
as usual, and verify acceptable media with the friend on a second PC. The tools
never sign in, join rooms, choose sources, capture pixels/audio, open a microphone,
change quality, install software or elevate themselves. They observe processes.

Obtain the source-only `gcp-gaming-measurement-tools` artifact from the PR's
successful **Gaming performance tooling tests** run, or use this branch checkout.
The artifact includes a source-commit/hash inventory and ZIP checksum, no app,
PresentMon, captures or fixture data. Hashes establish consistency, not publisher
identity. The Windows application needs no new dependency; analysis uses existing
standard-library Python tooling (3.12+) and collection uses Windows PowerShell
5.1 or PowerShell 7. Obtain PresentMon separately from its official project.

Keep operator data under the ignored `runs/` or `performance-evidence/` folder.
Start with one profile to inspect the method:

```powershell
python scripts/qualify-desktop-performance.py --init runs/manifest.json --scenario voice
```

Without `--scenario`, the template includes voice, voice+publish and voice+view.
It is **unfilled configuration**, not evidence: enter the tested 40-hex commit,
operator ID, exact game/scene/resolution/preset/FPS cap/power mode, PresentMon
version, media profile and budgets. Record voice mode/devices/peer count and screen
source size, FPS/bitrate and viewer size in the declared media profile. All budget defaults are null; the operator must deliberately
choose acceptable FPS/1%-low/p99, machine CPU, private-memory peak/growth and GPU
engine limits. Media quality must remain acceptable while meeting those limits.
The template's attestations start false and must reflect actual observations.

For each profile collect at least three distinct baseline/GCP pairs, with both
orders represented (for example baseline→GCP, GCP→baseline, baseline→GCP). Keep the
game scene, graphics/driver/thermal conditions and observer setup matched. Run all
pairs on one PC boot; the collector records a hashed machine/boot fingerprint,
hardware, game/executable hashes, QPC frequency and exact process identities.
Close unrelated GCP copies and diagnostics. Do not reuse another pair's files.

## Capture one condition

Record the actual game PID. For a baseline, close GCP and its native children in
this Windows session. Start PresentMon in another terminal with raw QPC times and
v2 metrics, for example (replace the executable, PID and paths):

```powershell
& C:\Tools\PresentMon.exe --process_id 1234 --qpc_time --output_file runs\voice\1\baseline.csv --timed 180 --terminate_after_timed --no_console_stats --no_track_input --no_track_display --no_track_gpu
```

Then collect a 120-second resource window entirely inside that frame capture:

```powershell
powershell -NoProfile -File scripts/capture-desktop-performance.ps1 -GameProcessId 1234 -Scenario baseline -DurationSeconds 120 -OutputFile runs\voice\1\baseline.jsonl
```

For the GCP condition, manually start the declared activity first, verify media
with the friend, and obtain the exact desktop PID. Start a fresh PresentMon CSV,
then run:

```powershell
powershell -NoProfile -File scripts/capture-desktop-performance.ps1 -GameProcessId 1234 -DesktopProcessId 5678 -Scenario voice -DurationSeconds 120 -OutputFile runs\voice\1\gcp.jsonl
```

Other labels are `voice_publish`, `voice_view`, `voice_publish_view` and optional
`idle`. The collector pins the exact desktop and all its known native children
in that Windows session; it rejects other GCP copies, unexpected children,
wrong process counts, exit/restart/PID reuse or changing executables. The same
screen binary implements publish/view: **the label and successful media activity
remain operator declarations**, not proof inferred from process presence.

For each CSV set its exact `SwapChainAddress` in the corresponding manifest entry.
Use `Import-Csv <file> | Group-Object ProcessID,SwapChainAddress | Select-Object Name,Count`
to inspect chains. The analyzer filters the recorded game PID and selected chain,
requires `CPUStartQPC` and `FrameTime`, and reports excluded rows. It rejects
mixed/out-of-order/duplicate or gapped selected timelines, short/truncated
windows, changed boots/builds, aliases/hard links/copy-reused contents, overlapping
captures and unmatched durations. No conversion from sampled FPS is accepted.
The example uses the [PresentMon 2.3 v2 CSV schema](https://github.com/GameTechDev/PresentMon/blob/v2.3.0/README-ConsoleApplication.md).
Newer default schemas can rename timing fields; use their `--v2_metrics`
compatibility option only if supported by the actual `--help`. Record the exact
version. Unsupported formats fail instead of guessing columns or clock units.

## Resource and timing interpretation

Collection writes bounded JSONL records without process command lines,
environments, tokens, full executable paths, raw frames or audio. Files are
created exclusively, never overwritten. A successful end record is mandatory;
interruption/process change leaves incomplete evidence that analysis rejects.
Sampling is nominally once per second with actual QPC intervals. Blocking Windows
telemetry calls can overrun a requested duration; sampling gaps/changed durations
are rejected during analysis, and the tool never stops the existing media call. Short smoke
captures are permitted by the collector but do not satisfy real-data analysis's
minimum 60-second window after warmup. Use longer sessions for memory stability;
a short slope does not establish freedom from leaks.

CPU uses cumulative process time deltas summed over desktop/native workers and
normalizes by elapsed time and the machine's logical processor count. Reported
memory is the sampled simultaneous sum of private committed bytes, plus a separate
summed working set (shared pages may be counted repeatedly). Peaks between samples
can be missed. The report includes mean/p95 CPU, private peak and fitted MiB/min
trend. Both CPU mean and p95, and every paired run, must meet the supplied limits.
The observer's own CPU/memory is reported separately. System services used by
sampling are not fully attributed to it; run the same observer in both conditions
and account for instrumentation overhead before making performance claims.
These process budgets exclude the server/SFU, compositor, kernel and shared audio
services; they do not measure total system/game CPU cost.

GPU observations use PID-filtered Windows GPU Engine counters. Selected processes
are summed **on the same adapter/physical engine**; the busiest such engine is
reported, never a sum of independent engines. Desktop/screen workers must have
usable instances throughout the analyzed window. Audio-only workers may have no
GPU instances; the metric covers observed attributed engines, not proof of all
hardware work. Unsupported/localized/denied/stale/missing counters leave the GPU
check incomplete, never a fabricated zero. `-SkipGpu` is useful for diagnostics
and CI smoke, and cannot satisfy the GPU gate. This does not measure GPU memory,
whole-system GPU load, hardware encoding correctness or quality.

The frame window is the resource window after warmup. Windows Stopwatch and
PresentMon's raw QPC values use the same local clock; no manual UTC offset aligns
the streams. Frame metrics describe the selected CPU/render frame pacing, not
remote screen latency, physical displayed FPS or audibility. Keep frame-generation
settings consistent; mixed/generated timelines that lack coherent CPU frame
starts cannot be qualified through this path.

Primary references: [Microsoft QPC/Stopwatch](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps),
[Microsoft GPU engine accounting](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/),
[Get-Counter limits/localization](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.diagnostics/get-counter?view=powershell-5.1),
and [official PresentMon console/CSV options](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md).

## Analyze and review

```powershell
python scripts/qualify-desktop-performance.py --manifest runs/manifest.json --output-dir runs/report-001
```

Use a new output directory. The report binds the manifest and each input by SHA-256, retains hardware and
declared game/media context, and rejects capture changes during analysis. It writes JSON and a Markdown summary only after the
analysis succeeds. Results are `limits_met` (exit 0), `regression` (2), `incomplete`
(3), or invalid input (1). These are numerical analysis outcomes, **not physical
certification**. Missing data or false operator media/scene declarations keep the
analysis incomplete. A fixture retains its fixture label and never becomes
physical evidence. Operator declarations are explicitly unverified, and all
physical-authentication/CI-performance-verification flags remain false.

The report distinguishes chosen profile coverage from the full product matrix.
`product_qualification_status` stays `not_qualified` for fixtures, incomplete or
partial scope; complete declared profiles within limits still require owner
acceptance. Review each run, media quality and actual PC behavior, not only an
aggregate. Do not claim superiority over another app without separately matched
measurements. Preserve the original captures and report with their build hashes.

The older voice-only benchmark remains available for native diagnostics. Its
single-process sampler now pins one PID/start time, rejects ambiguous names and
CPU resets, and refuses overwrite. File aliases/hard links and prior output
folders can no longer bypass its integrity checks. It does not measure the whole
Qt/native application; use this desktop path for Group 11.

## Evidence and remaining qualification

CI runs Python numerical/integrity fixtures, PowerShell 5.1/7 policy tests and
actual Windows **process-only fixtures** for collection, identity, interruption
and legacy output. Those fake game/desktop binaries never initialize Qt, LiveKit,
headsets or capture; GPU rows in unit tests are synthetic. Existing app/Qt/Rust,
SDK/SFU and packaging checks run separately. No actual owner gameplay data is
included or uploaded. The PR audit records exact heads, results and failures fixed.

The remaining Group 11 blocker is actual matched Windows captures, agreed
budgets, two-PC media/quality observations and owner review, followed by measured
optimization of any demonstrated bottleneck without reducing quality. The
existing viewer includes CPU-backed decode/copy/upload and still needs this
measurement. Group 12 attribution, owner signing and clean-PC/upgrade distribution
qualification remain separate. No merge, deployment, public release or new
installer implementation is included here.
