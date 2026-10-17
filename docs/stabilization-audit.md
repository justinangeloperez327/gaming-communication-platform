# Private Windows stabilization audit — 15 October 2026

Follow-up: server-side voice eviction is now implemented in [PR #65](https://github.com/justinangeloperez327/gaming-communication-platform/pull/65). See the [operating guide](server-voice-eviction.md) and that PR for current CI evidence; the dated evidence below describes the original audit revision.

## Decision

GCP has real API, Qt account/channel and native media integrations, but remains an experimental private test application. The portable ZIP pipeline exists. An installer should wait for the functional and hardware acceptance gates below; an installer would not resolve them. No service deployment, public release or installer was performed during this audit.

Scope: C++/Qt desktop controllers/QML, Rust API/auth/SQL authorization and migrations, native audio/capture/encoding/LiveKit boundaries, benchmark tools, workflows and Windows packaging. Findings combine source review and automated checks, not a claim of exhaustive defect elimination. Physical Windows audio, LiveKit networking, GPU driver behavior, remote playback and gaming performance were unavailable to this audit environment.

## Completed fixes

| Finding | Change | Verification |
| --- | --- | --- |
| Desktop response limits ran only after buffering entire HTTP bodies | Shared bounded-reply helper caps Qt buffers and aborts on excessive advertised or streamed data for login, channel lists and voice credentials | Real loopback Qt HTTP tests for advertised and unadvertised oversized bodies; Windows CTest |
| Voice shutdown depended on an event-loop watchdog that cannot run during destruction | Explicit bounded child-process cleanup for voice and device scan; scan startup failures now notify UI | Windows build; process/device hardware acceptance remains manual |
| Voice peak-memory sampling was declared but never executed | Restored two-second process-memory snapshots and sample counts | Source review and analyzer regression tests; physical memory profile not measured |
| Every actual voice log contained two identically named final records and was rejected by the benchmark analyzer | Generic metrics renamed `livekit_voice_process_summary`; exactly one final `livekit_voice_session`; reject unsampled/invalid metrics | Python combined-record regression and pipeline tests |
| Hardware CLI derived counts before checking FPS/duration | Validate arithmetic operands before multiplication/addition | Native self-tests include maximum signed integer input |
| GPU readback allocated after Map | Allocate first so allocation failure cannot strand a mapped staging texture | Source review; physical GPU failure injection unavailable |
| MFT failure could leak a returned provider-owned output sample | Adopt returned sample before checking HRESULT | Source review and Windows compilation |
| Capture setup failures could bypass event-pump stop | Enclose capture setup in existing stop/flush exception cleanup | Source review and Windows compilation |
| Hardware transport retained SDK publication references across shutdown | Destroy source/track/participant references before room destruction and SDK shutdown | Source review and Windows compilation; live SFU lifecycle still untested |
| Pre-encoded file read could grow beyond an earlier size check | Allocate/read the bounded original size and reject changed/truncated/extended input | Source review and Windows compilation |
| SDK exceptions could disclose credential-bearing details | Native transport/voice top-level errors are generic; raw SDK exception text is not echoed there | Source review; SDK's own warning logs are not a verified secret-redaction boundary |
| Session JSON and other private API replies lacked consistent cache control | Router response middleware adds `Cache-Control: no-store`; auth integration helper checks it | Rust API integration tests |
| Portable inventory used a two-backslash PowerShell literal for one-character path normalization | Consistent single-backslash → slash normalization in manifest/signature producers and verifier | Windows package and signature inventory checks |
| Package verifier could accept entries through directory junctions and ignored hidden extras | Reject reparse points including directories/root, unsafe wildcard paths and unmanifested hidden files | Windows fixtures: nested valid bundle, tamper, duplicate, missing required entry, traversal, wildcard, extra file and junction rejection |
| CI omitted direct main pushes and lacked portable sanitizer coverage | Main-push verification, superseded-run cancellation, explicit Qt Network discovery, native ASan/UBSan tests and integrity fixtures in regular CI | GitHub Actions |
| Desktop controls overflowed smaller windows; capability text was stale | Scrollable content, larger account panel and accurate experimental voice/API-only feature labels | QML startup in staged and extracted ZIP; visual layout not manually inspected |
| Documentation contradicted shipped code | Updated README, architecture, desktop/backend voice setup and stale subsystem claims; marked superseded voice milestones historical | Documentation/source consistency review |

No codec quality defaults, resolution/FPS/bitrate budgets, dependencies or runtime architecture were replaced. Resource improvements are bounded allocations, cleanup and valid instrumentation; no measured CPU/GPU/RAM reduction is claimed.

## Functionality and evidence

| Subsystem | What exists | What has not been demonstrated |
| --- | --- | --- |
| Accounts | Argon2id, hashed opaque tokens, atomic refresh rotation, revocation; desktop memory-only login/refresh | Physical desktop-to-live-backend acceptance; robust remote logout during network loss |
| Permissions | Membership/role/channel predicates in protected SQL; author/manager rules; block/friend constraints | Public deployment hardening or zero-latency revocation of operations already in flight |
| Text/events/DM | Persistent CRUD, pagination, single-process socket notifications, typing/presence, friends and blocks | Desktop chat UI, attachments, distributed fanout or DM socket delivery |
| Voice | Pinned native SDK platform audio, listen-only, explicit mic consent, device selection, room events, recovery watchdogs | Two users hearing each other; AEC/NS quality, Windows microphone privacy, headset unplug/default rerouting, long-call reliability |
| Screen | Explicit full-monitor consent, CPU readback publisher and GPU conversion/hardware-class H.264 transport diagnostic | Qt share/view UI, game-window picker, HDR/scaling/resize recovery, decoded quality and actual GPU encoder-engine activity |
| Video fixtures | Synthetic frames and pre-encoded file publisher | Actual screen capture, hardware use or gaming performance from fixture success |
| Distribution | Qt/QML deployment, LiveKit DLLs, hashes, signature inventory, staged/extracted runtime startup | Clean Windows consumer-PC startup, antivirus/SmartScreen behavior, complete third-party notices |

## Security and operational limits

- Microphone defaults off; publication requires explicit consent. Consent is reset after a join request. Voice channel JWTs permit microphone publication only, with no screen/camera/data grant. Screen diagnostics require independent explicit consent and operator-provided test credentials.
- Joining checks the current GCP session and community membership. Server reconciliation now invokes LiveKit RemoveParticipant when session or channel access is lost. Removal is periodic and retries after failures; self-hosted cached-token replay and control-plane outages remain limitations. See [voice eviction](server-voice-eviction.md).
- Every desktop call is capped at 300 seconds. Leave/sign-out stop local voice; remote logout is best effort. No in-call mute/push-to-talk or finished long-call experience exists.
- Selected devices are polled; OS-default-device swaps and Windows privacy enforcement need physical tests. Idle scans start no microphone publication but still initialize/enumerate the platform audio stack.
- Server registration is open to reachable clients; there is no invite-only account policy, complete login/IP throttling, email ownership verification, password recovery or MFA. The password-worker semaphore bounds Argon2 concurrency; it is not abuse protection. Use a private network/access boundary and HTTPS/WSS for the intended friend group.
- Credentials are not command-line arguments or committed configuration. Memory clearing is best effort; Qt implicit copies, SDK allocations and inherited child environment are not secure-zeroization guarantees. Avoid shared administrative access and do not publish diagnostic logs containing secrets.
- LiveKit participant identity is now scoped to the login session. Separate logins do not supersede each other; multiple connections using the same login identity still can.
- API traffic relies on an HTTPS reverse proxy outside loopback. Message content is plaintext to the API/database and media is not application E2EE.
- The downloaded SDK archive is pinned/checksummed. Manually supplied SDK paths require operator provenance checks. A local checksum manifest is consistency evidence, not a signature or trust anchor. Review Qt/LiveKit/third-party notices before binary sharing. Publisher signing remains the owner's choice for friends-only ZIP testing.

## Automated verification

Local Linux environment:

- Python standard-library suite: **29 tests passed**; Python compileall passed.
- H.264 Annex-B/access-unit and frame-arrival self-tests compiled with GCC C++20, `-Wall -Wextra -Werror`, AddressSanitizer and UBSan: **passed with leak detection disabled**. LeakSanitizer cannot inspect this sandbox's process/task state; no local leak-clean claim is made.
- `git diff --check`: passed.
- Cargo, CMake, Qt, PowerShell and Windows APIs were not installed locally. Rust/PostgreSQL and full Windows/package verification therefore use GitHub Actions, not local execution.

The initial audit revision `f5381f99501f8073f1bb9e88ffa1af90ac55dfa7` passed [CI run 38724614640](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/38724614640), including Rust formatting, Clippy with warnings denied, disposable PostgreSQL tests, Windows build/CTest, staged and extracted ZIP QML/SDK startup. Subsequent fixes require their own final revision checks; see the evidence update below and PR checks. Hosted Windows has development tooling installed, so this does not substitute for a clean-PC run.

## Final CI evidence

Code revision `6299c1f793e3baa8c8752e9693b9cbd265518d82` passed all workflows:

| Workflow | Result | Evidence |
| --- | --- | --- |
| Rust API + Windows Qt/media/LiveKit + private ZIP | Passed | [Run 38725727236](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/38725727236) |
| Windows package integrity fixtures | Passed | [Run 38725727239](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/38725727239) |
| Benchmark tooling tests | Passed | [Run 38725727235](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/38725727235) |

This covers Rust format/Clippy, 28 Rust tests with disposable PostgreSQL, 29 Python tests, Linux native ASan/UBSan self-tests, 15 Windows CTests, regular package rejection fixtures, signature inventory consistency, and staged/extracted ZIP Qt/QML and LiveKit SDK initialization. The only follow-up change to this tested tree records these results in this report. It does not imply a live SFU connection or hardware acceptance.

## Gates before installer work

1. Test the exact extracted ZIP on a clean Windows x64 machine with no Qt/Visual Studio development environment. Confirm platform/QML plugins, runtime dependencies, login and both native voice modes.
2. Complete two-user private-SFU tests: listen-only really does not transmit; mic capture stops on Leave/logout/process exit; both users hear audio; device loss, offline/online and reconnect behavior are acceptable.
3. Decide whether five-minute diagnostic calls meet the intended scope. For regular gaming sessions, implement/test long-call behavior, in-call mute and push-to-talk before treating this as a stable communication app.
4. Validate the implemented server-side eviction on a real LiveKit server during logout, membership removal, restart and network loss. Confirm the acceptable self-hosted cached-token rejoin window. Do not treat JWT expiry as eviction.
5. For screen sharing, finish the intended desktop consent/picker/viewer flow and independently test the GPU bridge on real hardware. Validate SDR dimensions, stop/cancel, remote decoding, text legibility, cadence and driver/device loss. Current voice-only backend tokens must not be broadened without explicit screen authorization design.
6. Capture matched gaming baselines and GCP voice/share runs (at least three each), including CPU/private memory, GPU Video Encode, FPS/1% lows, drivers, OS and headset/network details. Set budgets from evidence. Current voice metrics cover the child process, not complete desktop+server+game resource cost. Older broken/unsampled voice logs must be recaptured.
7. Review third-party redistribution/license notices, version/architecture metadata and checksum delivery. Decide publisher signing separately. Plan installer upgrade/uninstall/rollback only after the portable application passes acceptance.

No physical two-user voice, screen sharing, clean-PC compatibility or gaming performance acceptance gate is closed by this report.
