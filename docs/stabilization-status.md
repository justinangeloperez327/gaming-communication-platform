# Current stabilization audit — Group 10

GCP remains an experimental private Windows communication application. Groups
1–5 are merged; [Group 6 / #84](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84),
[Group 7 / #85](https://github.com/justinangeloperez327/gaming-communication-platform/pull/85),
[Group 8 / #86](https://github.com/justinangeloperez327/gaming-communication-platform/pull/86),
[Group 9 / #87](https://github.com/justinangeloperez327/gaming-communication-platform/pull/87)
and [Group 10 / #88](https://github.com/justinangeloperez327/gaming-communication-platform/pull/88)
form the current stack. Merge prerequisites before their dependent PRs. This
work merges none of them. Groups 8/9 implement the source picker/publisher and
explicit desktop viewer; actual physical media acceptance remains pending. The existing unsigned alpha
installer is an installation preview. No release, deployment, version change or
new installer implementation is included.

## Completed fixes

| Area | Implemented | Evidence and limits |
| --- | --- | --- |
| Shared server | Generated secrets, migrations, readiness/restarts, TLS configuration, persistence and backup/restore | PR #79; real-container acceptance, no external deployment |
| Accounts | Local stdin owner bootstrap, hashed email-bound one-use invitations, atomic invited signup and desktop owner controls | PR #80; PostgreSQL/Qt/container tests; no email delivery, recovery, MFA or persistent login |
| Communities | Desktop create/join/leave, channel CRUD/order, invitations/member roles and selection-preserving refresh | PR #81; API/Qt coverage; capped lists, no live directory updates or owner transfer |
| Voice | Per-call consent, muted/PTT-disarmed interruption recovery, current-access recheck, bounded token rotation and native cleanup | PR #82; Qt/process tests and real SDK/SFU synthetic PCM, mute/isolation/rejoin/active-eviction contract; owner two-PC/device acceptance pending |
| Desktop text | Independent selection, paginated history, Unicode plain-text composer, author edits, confirmed role-aware deletion and live ID refetch | PR #83; QML/controller/HTTP/socket and real Qt→Rust→PostgreSQL acceptance; no physical friend-user claim |
| Message integrity | Stable optional UUID retries, current edits/tombstones, microsecond ordering, bounded refetch and gap recovery | Concurrent PostgreSQL and Qt loss/rotation/revocation tests; no durable offline outbox or cross-server events |
| Friends/private messages | Request/cancel/accept/decline, block/unblock, conversations and shared composer with private live delivery | PR #84; shipped QML, real Qt→Rust→PostgreSQL and three-account socket/authorization tests |
| Private authorization | Ordered pair locks for writes/revocation, current friendship/bilateral blocks, author-only mutation and isolated event hubs | Equal channel/conversation UUIDs, concurrent retry IDs, tombstones, outsider denial and logout tests |
| Screen authorization | Screen room isolated from voice, separate publisher/viewer identities and 60-second leases, explicit API acknowledgements, restricted JWTs, trusted ownership lookup and Stop/expiry reconciliation | PR #85; PostgreSQL/API/control fixtures plus a separate real SDK/SFU synthetic-video contract; Groups 8/9 connect consent, publishing and viewing; physical acceptance remains pending |
| Screen publisher | Exact Windows picker item retained in native child, Qt source/channel confirmation, GPU hardware H.264, renewable leases and bounded local stop | PR #86; Qt/QML/HTTP/process policy and real Qt→Rust→PostgreSQL lease acceptance; actual OS/GPU capture and physical quality unverified |
| Capture lifecycle | Serviced asynchronous encoder output during static input waits, elapsed-time capture timestamps, bounded input/event backlog and synchronized capture teardown | Compiled Windows path; GPU behavior and resource savings still need physical acceptance |
| Screen viewer | Explicit authenticated discovery/Watch, backend exact-SID subscription, native receive-only process, Qt SceneGraph, credit-bounded frames and terminal interruption/visibility cleanup | PR #87; rendered synthetic pixels with a protocol child, real Rust track authorization and separate real SDK/SFU production receiver core; physical end-to-end remains unverified |
| Settings/recovery | Atomic bounded audio IDs/PTT preferences, opt-in server origin, explicit missing-device/Default handling, readiness/session checks and safe relogin | PR #88; Windows/Linux file/network/QML and real Qt→Rust readiness/rotation checks; no credential or consent persistence and no physical device verification |
| Resources | Virtualized/capped text models, bounded requests, frames, queues, HTTP/socket replies and timeouts; two screen leases per login | Explicit limits/regressions; no measured gaming resource reduction or exhaustive leak-free claim |
| Distribution | Required Qt WebSockets and native voice/screen payloads, ZIP integrity and extracted/installed startup, signing inventory and existing alpha installer lifecycle | Hosted Windows checks; full attribution, owner signing, consumer-PC/version-upgrade qualification pending |

The C++20/Qt, Rust/Axum/PostgreSQL and native LiveKit architecture remains.
Groups 7–10 add no third-party runtime dependency, SDK update or broker. Group 7's explicit
migration adds screen leases. Separate screen rooms prevent voice subscriptions
from silently receiving screen tracks; voice JWTs remain microphone-only.
Group 8 reuses the existing GPU capture/converter/hardware encoder through an
additional controlled-session entry point, preserving finite diagnostics. The
initial desktop profile is SDR up to 1920×1080/30 fps, 4 Mbps initial H.264 target.
No software encoder, CPU raw-pixel path, system audio or HDR tone mapping is added.

## Verified functionality versus remaining work

| Subsystem | Automated execution | Remaining work or acceptance |
| --- | --- | --- |
| Accounts/community | Real database/HTTP/container onboarding and role checks plus offscreen desktop controls | Deployed host, recovery/email, owner transfer, live directory updates and pagination |
| Voice controls/transport | Actual Qt controls with controlled native child; separate real SDK/SFU synthetic PCM | Complete deployed desktop→API→PlatformAudio→friend speakers; physical privacy/audibility/F8/devices, SDK network interruption and long gaming calls |
| Text/friends/DMs | Shipped QML/controller protocol tests, real two-account Qt/Rust/PostgreSQL delivery, concurrent retries and bilateral revocation | Physical/deployed acceptance; push/unread/search/attachments/typing UI, durable outbox and cross-server events |
| Screen authorization | Real PostgreSQL/API signatures, consent/role/access/ownership/expiry/stop tests; real SDK/SFU source rejection, synthetic decode, room isolation and selected publisher removal in a separate execution | Physical OS capture, remote quality and GPU/driver acceptance |
| Screen publisher | Shipped Qt/QML plus real subprocess and HTTP failure fixtures; real Rust-issued lease create/renew/stop/access removal with a protocol-only native peer | Windows picker permissions/content, actual GPU encoder publishing, physical receiver and real SDK/network interruptions |
| Windows distribution | Hosted staged/extracted/installed runtime and dependency-integrity checks | Consumer Windows PC, upgrades, complete transitive licensing/source obligations and owner publisher signing |
| Desktop recovery | Real settings files, bounded Qt readiness/probe faults, missing-device/process fixtures and real Rust session rotation | Actual consumer-profile persistence, USB/Bluetooth/privacy permissions and network failure acceptance |
| Performance | Telemetry/export tools and fixture validation | Matched actual gaming CPU/GPU/RAM/frame-time measurements; no measured budgets yet |

## Security and consent boundaries

Protected operations use current database sessions and community/friend access.
Community roles never grant DM moderation. Private/channel event payloads carry
IDs, not message bodies or credentials, and clients reauthorize current records.
All API requests remain on the approved origin with bearer headers. Signing
secrets/admin grants stay server-side; the public issuer identifier is included
in participant JWTs. Credentials belong in no URL, log or native command line.
Messages are plain text to the server/database, without application E2EE.

Registration requires owner invitations; email verification, recovery, MFA and
complete public-internet abuse controls remain absent. Operate within a private
network boundary and HTTPS/WSS. Text fanout remains single process with no durable
replay; drafts remain memory-only and Retry is explicit. A friend block does not
isolate shared community text, voice or screen access.

Microphone capture requires per-call consent, muted recovery and explicit
rearming; mute/PTT may leave the recording device open until Leave. Screen API
acknowledgements cannot prove OS consent. The desktop now retains the exact
Windows picker result in its child and requires Qt confirmation for that
source/account/channel before requesting a token. Cancellation, source closure
or resize, logout/scope changes, lost lease or SDK interruption require a fresh
selection and consent. This implementation still needs physical OS acceptance.
A screen-only Stop invalidates its lease without removing voice. Reconciliation
also rejects extra/non-screen-video publisher tracks and any viewer publication.
LiveKit source labels do not prove the content is a screen, and an authorized
viewer can record it. No application E2EE or remote control is added.

Both media namespaces share the bounded periodic reconciler. Token expiry does
not itself disconnect established peers. SFU/database outages delay enforcement;
self-hosted cached JWTs can rejoin before expiry and be removed on a later pass.
This is not gap-free revocation. See [screen authorization](screen-authorization.md),
[voice acceptance](usable-voice.md) and [server eviction](server-voice-eviction.md).

Package hashes prove consistency, not publisher identity. The notice catalog is
a baseline, not complete attribution. No stable-distribution approval is implied.

## Checks and evidence

The [PR #88 checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/88/checks)
record the exact tested head and each required result. The PR description is the
checked-head audit record, including run links, counts and any failures fixed;
this maintained guide records capability and acceptance boundaries. The editing host has no
Rust/Qt/Windows toolchains; those checks execute in CI. Local portable native
control/parser checks and all 50 Python tests pass. Address/undefined sanitizer
checks run locally with LeakSanitizer disabled because this host runs under
ptrace; the normal full sanitizer invocation remains in CI.

Required checks cover Rust format/Clippy/PostgreSQL, sanitizer/Python suites,
real Linux SFU control, real container acceptance, Windows Qt/native build and
CTests including actual viewer rendering, separate synthetic PCM/video SDK/SFU contracts, staged/extracted/installed
payload checks and real Qt→Rust messaging/screen lease integration. Test native
peers are not bundled. New payload checks require `gcp-livekit-screen.exe` and
exercise its no-device self-test after staging, extraction and installation.

The Qt/Rust screen acceptance uses an explicitly simulated native protocol peer;
it does not capture/publish pixels. The separate SDK/SFU test decodes actual
synthetic frames but does not combine Rust authorization, this Windows picker,
GPU capture and a physical desktop viewer in one run. Group 9 adds a separate
production receiver-core SDK/SFU test, still using synthetic pixels. Passing any test does not establish
physical voice/screen, consumer-PC or gaming acceptance. See
[desktop screen publisher](desktop-screen-publisher.md) for each boundary and
owner-run checks.

Graphify traced JWT sources, diagnostic capture consent, participant identities
and eviction/test dependencies at indexed commit `9b745bd` (before Group 6).
Current source and executable checks were reviewed independently; the graph is
navigation evidence, not runtime proof.

## Blockers before stable installer qualification

An alpha installer already exists from the owner's earlier request. Group 8
retains its lifecycle checks and updates the shared payload with the screen
process. A stable installer still needs consumer-PC settings/recovery checks,
physical publisher/viewer and deployed two-PC media acceptance, and measured
gaming qualification. It also needs complete dependency attribution, owner code signing,
clean consumer-PC startup and version-upgrade evidence. Neither another release
nor a deployment is included in this group. See the [completion roadmap](completion-roadmap.md).


Group 10’s [settings/recovery guide](desktop-settings-recovery.md) records the
non-secret preference schema, optional address storage and explicit device
fallback policy. Missing IDs are retained and block affected joins, never
silently replaced. Recurring idle scans are removed; returning devices do not
rejoin media. Manual session checks rotate once and fail closed on uncertain
replies; they add no password-reset/email recovery or persistent login.
The next roadmap stage is Group 11 actual gaming qualification, followed by
Group 12 signing/attribution/consumer-PC distribution qualification.
