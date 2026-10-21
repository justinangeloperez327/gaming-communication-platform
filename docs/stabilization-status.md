# Current stabilization audit — 21 October 2026

GCP remains an experimental private Windows communication application. Automated
checks cover real backend logic, desktop HTTP/QProcess behavior, native policy
logic and portable packaging. They do not establish stable two-user audio,
desktop screen sharing or gaming resource budgets. Installer work remains blocked
by the acceptance items below.

This report reviews main revision `4866d9772b79b9a07ae10220f45644f17641cf41`, whose application tree
matches tested PR head `d73d37324419020fcb63502730fab4b651c61153` from PR #75.
The [15 October report](stabilization-audit.md) is historical; its five-minute
call, missing mute/PTT and original test-count statements are superseded here.
This is a source and automated-test audit, not exhaustive defect elimination.

## Completed fixes and evidence

| Area | Completed work | Evidence and limits |
| --- | --- | --- |
| Initial audit | Bounded HTTP bodies, child cleanup, native capture/encoder exception cleanup, corrected memory/benchmark records, package traversal/hidden-file rejection and sanitizer CI | [PR #63](https://github.com/justinangeloperez327/gaming-communication-platform/pull/63); historical report details each finding. No physical leak-free or gaming-performance claim |
| Long calls and microphone controls | Managed calls continue until Leave/failure; initial mute, F8 PTT, heartbeat lease, mute acknowledgment watchdog and periodic backend access checks | [PR #64](https://github.com/justinangeloperez327/gaming-communication-platform/pull/64), [guide](long-session-voice.md). Synthetic 12-hour policy timeline and offline subprocess tests are not a multi-hour audio call |
| Server room access | Session-specific participant identities, periodic database/SFU reconciliation, RemoveParticipant, readiness gating, bounded control requests and retries | [PR #65](https://github.com/justinangeloperez327/gaming-communication-platform/pull/65), [guide](server-voice-eviction.md). Active-media eviction and cached-token rejoin timing remain unverified |
| LiveKit control compatibility | Dedicated test against checksum-pinned LiveKit 1.13.10 on loopback | [PR #66](https://github.com/justinangeloperez327/gaming-communication-platform/pull/66). Exercises actual room-service authorization/contracts with no media peers |
| Native output and credentials | Bounded device scan/status stdout, shutdown on oversized status lines, discarded unused stderr, removal of known backend credentials from child environments | [PR #67](https://github.com/justinangeloperez327/gaming-communication-platform/pull/67), [#68](https://github.com/justinangeloperez327/gaming-communication-platform/pull/68), [#71](https://github.com/justinangeloperez327/gaming-communication-platform/pull/71). Dummy-environment and real QProcess tests; not secure zeroization of SDK/process memory |
| Native URL safety | Shared authority validation rejects deceptive loopback URLs across native media tools | [PR #69](https://github.com/justinangeloperez327/gaming-communication-platform/pull/69), portable policy tests. No TLS deployment acceptance claim |
| Realtime fanout | Channel-scoped WebSocket queues and RAII subscriber cleanup replace cross-channel fanout | [PR #70](https://github.com/justinangeloperez327/gaming-communication-platform/pull/70), backend/concurrent-drop tests. Single-process delivery only |
| Notice packaging | Exact pinned Qt/LiveKit baseline texts, independent notice hashes, version guards and rejection fixtures | [PR #72](https://github.com/justinangeloperez327/gaming-communication-platform/pull/72), [coverage](windows-third-party-notices.md). Full transitive attribution review remains open |
| Desktop HTTP lifetime | Logout response size cap; absolute reply-owned deadlines for all desktop HTTP calls in addition to inactivity timeouts | [PR #73](https://github.com/justinangeloperez327/gaming-communication-platform/pull/73), [#74](https://github.com/justinangeloperez327/gaming-communication-platform/pull/74), actual loopback oversized/silent/trickling-response tests. Deadlines require a responsive Qt event loop |
| Device selection | Dropdowns follow controller endpoint IDs across model replacement/reordering; scans preserve or clear selection consistently | [PR #75](https://github.com/justinangeloperez327/gaming-communication-platform/pull/75), actual offscreen QML and subprocess argument tests. Physical endpoint opening remains unverified |

No runtime dependency or core Qt/Rust/LiveKit architecture was replaced by these
fixes. Bounded allocations, reduced cross-channel fanout and cleanup remove known
resource risks; no measured reduction in CPU, GPU, RAM or game frame times is
claimed. Audio/codec quality settings were preserved.

## Capability status

| Subsystem | Implemented and automatically exercised | Remaining functionality or acceptance |
| --- | --- | --- |
| Account and authorization | Argon2id, hashed bearer tokens, rotating refresh, revocation, database membership/role/channel predicates; desktop in-memory login/refresh/logout | Desktop-to-deployed-backend acceptance; no persistent login, invite-only registration or complete public-internet abuse protection |
| Communities and text | Backend invitations, channels, messages, friendships, blocks, DMs and text-channel socket events; desktop voice directory | Community administration/invitations and text/friend/DM workflows remain API-only; composer is disabled; no distributed fanout |
| Voice | Native SDK platform audio, listen-only and per-call mic consent, mute/PTT, device choices, SDK events, watchdogs and access rechecks | Remote audibility, Windows microphone privacy, headset/default-device behavior, long calls and late reconnect while gaming |
| Screen | Explicit consent-gated full-monitor diagnostics; CPU readback publisher and GPU conversion/hardware-class H.264 transport bridge | Desktop sharing/viewing UI, window picker, independent screen authorization flow, remote decoded quality and actual GPU encoder-engine evidence |
| Synthetic media | Generated frames and externally encoded file publishing | These are transport/format fixtures, not screen-capture, hardware-encoder or performance verification |
| Portable ZIP | Pinned Qt/SDK, deployed plugins/DLLs, payload hashes, signing inventory, staged/extracted runtime startup and notice baseline | Clean consumer Windows PC, full attribution/source obligations, owner signing choice and physical media acceptance |

## Security and consent limits

- Microphone publication requires per-call consent and starts muted. Mute/PTT
  controls transmission; the device/source may remain open until Leave. Track
  acknowledgments do not prove remote audibility or Windows privacy behavior.
- Voice tokens grant microphone publication only. Screen diagnostics require
  separate explicit consent and operator test credentials. A finished desktop
  screen-sharing feature must add its own authorized consent/picker/viewer flow.
- Desktop access rechecks stop a cooperative client on failure. Server
  reconciliation separately removes unauthorized participants periodically.
  Outages can delay removal; self-hosted cached JWTs can rejoin before expiry.
  Token expiry alone does not evict an established participant.
- Separate logins have separate participant identities. Refresh preserves the
  same login identity; concurrent connections using that login can still collide.
- Use a private access boundary for the intended friends and HTTPS/WSS outside
  loopback. Registration is open to reachable users. Complete rate limiting,
  email verification, recovery and MFA are not implemented.
- Server admin credentials stay out of desktop-issued grants. Known backend
  credential environment variables are filtered from native children; this is
  not an exhaustive environment allowlist or secure-memory guarantee.
- Messages are plaintext to the API/database. Application-level E2EE is absent.
  Package checksums prove consistency, not publisher authenticity.

## Verification record

Application main: `4866d97` (merge of PR #75); tested application head:
`d73d37324419020fcb63502730fab4b651c61153`. Their tracked trees match.
[CI run 39540102932](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/39540102932)
passed both jobs, including:

- Windows Qt/media/LiveKit compilation and **19 CTests**. The actual QML picker
  test and HTTP/QProcess voice control tests passed.
- Package corruption/notice rejection fixtures, signing-inventory consistency,
  staged and extracted Qt/QML and SDK startup; **1,327 payload files** verified
  in that build. Counts are build evidence, not permanent package contracts.
- Rust formatting, Clippy with warnings denied, PostgreSQL-backed tests,
  explicitly selected real LiveKit control contract, portable native sanitizer
  checks and Python suites. The external-server contract is ignored by regular
  Cargo tests and run separately in CI.

Local checks during these follow-ups include 29 Python tests, authored-file
whitespace checks and GCC C++20 fixture compilation with warnings denied. Qt,
Rust and PowerShell execution used CI rather than the local audit environment.
Preserved upstream notice EOF whitespace is intentional.

Graphify's available index was `10122d6`, before PR #75. It helped locate the
directory/voice boundaries; current source and CI were checked independently.
Its semantic symbol suggestions and name-linked tests are not proof of a Qt
signal/slot call path or complete coverage. No runtime verdict is inferred from
graph topology.

## Blockers before implementing the Windows installer

1. Test the exact extracted ZIP on clean Windows x64 without Qt/Visual Studio
   development tooling. Confirm dependencies, account flow and native voice.
2. Complete the owner's two-user private-SFU acceptance: consent/listen-only,
   both users hearing audio, mute/PTT, Leave/logout/exit, device loss, long calls
   and network recovery before/after JWT expiry.
3. Test actual connected-participant eviction/rejoin after logout, membership or
   channel removal and backend/SFU restart/outage. Set acceptable revocation
   timing from observations; the empty-room control contract does not close this.
4. Finish the intended desktop screen-sharing consent/authorization/picker/viewer
   integration and test capture, stop, decoding, readability, cadence, device loss
   and supported display modes on real hardware.
5. Measure matched baseline and GCP voice/share gaming runs, including complete
   desktop/native resource cost, CPU/private memory, GPU Video Encode, FPS/1% lows,
   hardware/drivers/network details and repeated runs. Agree budgets from evidence.
6. Finish exact-build Qt/plugin and LiveKit FFI/WebRTC/codec attribution/source
   review, plus Microsoft runtime redistribution material. The pinned baseline
   is incomplete by design. Publisher code signing remains the owner's task.

No installer, public release or service deployment was created. Installer
upgrade/uninstall/rollback work should follow portable application acceptance.
