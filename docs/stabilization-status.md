# Current stabilization audit — Group 4

GCP remains an experimental private Windows communication application. Groups
1–3 are merged; [Group 4 / PR #82](https://github.com/justinangeloperez327/gaming-communication-platform/pull/82)
adds voice consent/recovery fixes and a real synthetic-audio SDK/SFU contract.
The owner's physical two-PC voice acceptance remains pending. The existing
unsigned alpha installer is an installation preview, not product acceptance.
No new release, service deployment, installer implementation or merge is part of
Group 4.

## Completed fixes

| Area | Implemented | Evidence and limits |
| --- | --- | --- |
| Shared server | Generated secrets, explicit migrations, readiness/restarts, restricted TLS configuration, persistence and backup/restore | Merged [PR #79](https://github.com/justinangeloperez327/gaming-communication-platform/pull/79), [server guide](server-stack.md); real-container acceptance, no external deployment |
| Accounts | Local stdin owner bootstrap, one-use hashed email-bound invitations, atomic invited registration and desktop signup/owner invitation UI | Merged [PR #80](https://github.com/justinangeloperez327/gaming-communication-platform/pull/80), [onboarding](account-onboarding.md); PostgreSQL, Qt and container tests; no email delivery/recovery/MFA/persistent login |
| Communities | Desktop create/join/leave, channel CRUD/order, member roles and invitation issue/revoke; bounded requests/models and selection-preserving refresh | Merged [PR #81](https://github.com/justinangeloperez327/gaming-communication-platform/pull/81), [guide](desktop-communities.md); real API/container and offscreen QML tests; no live directory updates, pagination or owner transfer |
| Voice recovery | An interruption clears open-mic/PTT arming, cancels old checks, mutes the track and requires immediate current-access reauthorization plus Enable mic after SDK recovery | [PR #82](https://github.com/justinangeloperez327/gaming-communication-platform/pull/82), [voice report](usable-voice.md); Qt HTTP/QProcess and portable policy tests; physical network interruption remains unverified |
| Voice consent/lifetimes | Consent clears on either join, Leave, logout and room/process transitions; connected acknowledgment waits for audio publication; terminal errors survive exit; pending replies abort before destruction; stopping closes unused stdout | Shipped VoicePanel is exercised in Qt tests, including delayed readiness, denial, missing mute acknowledgment, cancellation/destruction and fast child failure; physical capture/privacy remains pending |
| Media integration | Real synthetic PCM publisher/receiver, decoded tone, initial mute, remote mute/unmute, room isolation, explicit rejoin and active-peer RoomService eviction assertions | Dedicated Windows loopback contract with pinned existing SDK and checksum-pinned SFU; no PlatformAudio, real accounts, forced SDK network interruption or physical devices |
| Existing safety work | Bounded HTTP/output, subprocess/native cleanup, five-second control lease, periodic access checks, session-specific identities, SFU reconciliation/readiness, native URL policy and channel-scoped realtime queues | [Earlier audit](stabilization-audit.md), [long calls](long-session-voice.md), [eviction](server-voice-eviction.md); no exhaustive defect-free or leak-free claim |
| Distribution | Qt/QML/LiveKit staging, ZIP inventory/hashes, corruption rejection, notice baseline, signature inventory and existing alpha install/repair/uninstall | Existing Windows CI checks are retained; no version bump or new release; clean consumer-PC and full attribution/signing remain open |

The Qt/C++ desktop, Rust/Axum/PostgreSQL backend and native LiveKit architecture
and audio quality defaults are preserved. No new application runtime dependency
was added. Lifecycle and bounded-buffer fixes remove known resource risks; no
measured CPU/GPU/RAM or gaming frame-time improvement is claimed.

## Verified behavior versus unfinished functionality

| Subsystem | Automated execution | Remaining work or acceptance |
| --- | --- | --- |
| Accounts/community | Real database/HTTP/container onboarding and role checks plus actual offscreen desktop controls | Deployed-host/physical desktop acceptance; recovery/email/owner transfer/live directory updates |
| Voice controls | Real Qt/QML/HTTP/QProcess behavior with a simulated native child; native control policy separately under sanitizers | Physical microphone privacy, audibility, F8/game compatibility, default-device routing and multi-hour calls |
| Voice transport | Dedicated real SDK/SFU synthetic PCM contract; Rust grants/reconciliation separately covered | Full deployed desktop → API → PlatformAudio → friend speakers; actual SDK network recovery and revocation/rejoin timing |
| Text/friends/DMs | Authorized persistent API operations and channel socket events | Desktop composer/history/live delivery and friend/DM flows remain incomplete; composer is disabled |
| Screen sharing | Consent-gated monitor/capture/encoder/transport diagnostics and format/parser tests | Desktop consent/picker/publisher/viewer authorization and physical remote decode/quality remain incomplete |
| Windows packages | Hosted build, runtime/dependency and existing installer lifecycle tests | Clean Windows consumer PC, upgrade qualification, full transitive licensing/source obligations and owner publisher signing |
| Performance | Corrected telemetry and analysis tools, fixture validation | Matched gaming measurements of complete desktop/native resource cost and frame-time impact; no measured budgets yet |

## Security and consent boundaries

- Microphone capture requires per-call consent. Calls and mode changes start
  muted; interrupted calls must be explicitly armed again. Mute/PTT controls
  transmission, and the device may stay open until Leave. Hosted tests do not
  establish Windows privacy or physical capture-stop behavior.
- The desktop's immediate/periodic HTTP access checks govern the cooperative
  client. Rust separately reconciles real SFU participants against current
  sessions, membership and channel state. Outages can delay removal; cached
  self-hosted JWTs can rejoin before expiry. Expiry alone does not evict a peer.
- Voice JWTs permit microphone publication only. Screen diagnostics use separate
  explicit consent and operator credentials; desktop screen authorization is
  still a later group. Separate logins have separate identities; refreshing a
  login preserves its identity, so concurrent connections on that login collide.
- Registration now requires a valid owner-issued invitation. Email ownership,
  recovery, MFA and complete public-internet rate limiting are absent. Use a
  private network boundary and HTTPS/WSS for the intended friend group.
- Backend credentials stay server-side. Known credential variables are removed
  from native child environments; this is not a sandbox or secure zeroization.
  Messages are plaintext to the API/database and application E2EE is absent.
- Package hashes prove consistency. Owner signing and full attribution/source
  obligations remain separate from the current notice baseline.

## Checks and evidence

PR #82 checks identify the exact tested head. Its CI must pass Rust formatting,
Clippy with warnings denied, real PostgreSQL tests, the explicit Linux SFU
control contract, sanitizer tests, Python suites, real container acceptance,
Windows native/Qt build and CTest, the new real synthetic-audio contract and
existing package/install lifecycle checks before it is marked ready. Workflow
success is not physical voice, screen, clean-PC or gaming acceptance.

The first Group 4 implementation head
`60e538ecb45077a13420feca40c9824e99ebd45e` passed all three jobs in
[CI 39849493873](https://github.com/justinangeloperez327/gaming-communication-platform/actions/runs/39849493873):
21 Windows CTests, Rust/PostgreSQL/sanitizer/Python checks, real container
acceptance, synthetic media and existing ZIP/preview-installer checks. The media
contract recorded 1,124 decoded frames, including 844 tone frames; the staged
package verified 1,339 payload files. Counts describe that run, not fixed product
budgets. The subsequent source-owner lifetime/audio-readiness/documentation
follow-ups must also pass the current PR head's checks; final run links are
recorded in PR #82.

Group 4 local checks passed 49 Python tests, whitespace checks and GCC C++20
voice-control ASan/UBSan with leak detection disabled: this environment's
LeakSanitizer cannot inspect process/task state. Qt/Rust/Docker/Windows checks
use CI because those runtimes are unavailable locally.

Graphify was used to trace the voice/lifecycle/test boundaries. Initial recall
used `1285b7c`; its later impact query had refreshed to main `2e4d100`, still before
this branch. Current source was checked independently.
Recalled candidate reviews and graph relationships are not runtime evidence.

## Remaining qualification before stable distribution

The owner already authorized the preview installer, which exists. These are
product/stable-distribution blockers, not requests to implement another installer:

1. Complete the [two-PC voice/device/privacy/PTT/network acceptance](usable-voice.md)
   on the exact build, including long calls and interruption after token expiry.
2. Verify occupied-room logout/membership/channel revocation with the complete
   Rust reconciliation loop and actual users, including outages and copied-token
   rejoin timing. The synthetic RoomService test verifies active-peer removal,
   not every authorization transition in one end-to-end run.
3. Finish desktop text, friends/DMs and screen authorization/publisher/viewer
   groups from the [roadmap](completion-roadmap.md). Test physical screen capture,
   stop/consent, remote readability/cadence and device/display changes.
4. Measure repeated matched gaming baseline/voice/share runs: full desktop/native
   CPU/private memory, GPU engines and FPS/1% lows with hardware/driver/network
   details. Set budgets from evidence before changing quality settings.
5. Test the exact extracted ZIP on clean Windows x64 without Qt/Visual Studio,
   then qualify preview install/repair/uninstall and upgrades on consumer PCs.
6. Complete exact-build Qt/plugin/LiveKit FFI/WebRTC/codec and Microsoft runtime
   attribution/source review; the owner handles publisher code signing.

No physical two-user voice, screen sharing or gaming result is claimed here.
