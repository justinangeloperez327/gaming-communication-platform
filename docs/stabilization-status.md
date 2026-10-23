# Current stabilization audit — Group 7

GCP remains an experimental private Windows communication application. Groups
1–5 are merged. [Group 6 / PR #84](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84)
adds desktop friends/DMs and is the unmerged base of
[Group 7 / PR #85](https://github.com/justinangeloperez327/gaming-communication-platform/pull/85).
Group 7 implements screen authorization before Groups 8/9 connect desktop capture
and viewing. Neither branch is merged by this work. The current unsigned alpha
installer remains an installation preview; no release version, deployment or
installer implementation changes in this group.

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
| Screen authorization | Separate publisher/viewer rooms, identities and 60-second leases, explicit API acknowledgements, restricted JWTs, trusted ownership lookup and Stop/expiry reconciliation | PR #85; PostgreSQL/API/control fixtures plus a separate real SDK/SFU synthetic-video contract; desktop consent/capture/view integration remains Groups 8/9 |
| Resources | Virtualized/capped text models, bounded requests, frames, queues, HTTP/socket replies and timeouts; two screen leases per login | Explicit limits/regressions; no measured gaming resource reduction or exhaustive leak-free claim |
| Distribution | Required Qt WebSockets dependency, ZIP integrity and extracted/installed startup, signing inventory and existing alpha installer lifecycle | Hosted Windows checks; full attribution, owner signing, consumer-PC/version-upgrade qualification pending |

The C++20/Qt, Rust/Axum/PostgreSQL and native LiveKit architecture remains.
Group 7 adds no runtime dependency or broker. One explicit migration adds screen
leases. Separate screen rooms are necessary because LiveKit subscription grants
cover an entire room; voice must not silently receive screen tracks. Voice JWTs
remain microphone-only. Media-quality defaults and diagnostics are unchanged.

## Verified functionality versus remaining work

| Subsystem | Automated execution | Remaining work or acceptance |
| --- | --- | --- |
| Accounts/community | Real database/HTTP/container onboarding and role checks plus offscreen desktop controls | Deployed host, recovery/email, owner transfer, live directory updates and pagination |
| Voice controls/transport | Actual Qt controls with controlled native child; separate real SDK/SFU synthetic PCM | Complete deployed desktop→API→PlatformAudio→friend speakers; physical privacy/audibility/F8/devices, SDK network interruption and long gaming calls |
| Text/friends/DMs | Shipped QML/controller protocol tests, real two-account Qt/Rust/PostgreSQL delivery, concurrent retries and bilateral revocation | Physical/deployed acceptance; push/unread/search/attachments/typing UI, durable outbox and cross-server events |
| Screen authorization | Real PostgreSQL/API signatures, consent/role/access/ownership/expiry/stop tests; real SDK/SFU source rejection, synthetic decode, room isolation and selected publisher removal in a separate execution | Desktop source picker, actual local consent enforcement, publisher and viewer: Groups 8/9; physical remote capture/quality/driver acceptance |
| Windows distribution | Hosted staged/extracted/installed runtime and dependency-integrity checks | Consumer Windows PC, upgrades, complete transitive licensing/source obligations and owner publisher signing |
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
acknowledgements cannot prove that Windows showed a picker. The upcoming desktop
must bind consent to an exact source/account/channel and stop on interruption.
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

The [PR #85 checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/85/checks)
record the exact tested head. Required checks include Rust formatting/Clippy and
real PostgreSQL tests, sanitizer/Python suites, Linux LiveKit control and real
container acceptance, Windows Qt/native build with 24 CTests, real synthetic PCM
and screen authorization contracts, ZIP/package/installed-runtime checks, and
real Qt→Rust channel/friend/DM integration. The branch remains draft until checks
pass. The editing host has no Rust/Qt toolchains; those checks execute in CI.

The Windows screen contract uses fixture JWTs matching the separately tested
Rust claims. It does not combine a Rust-issued lease, native capture and desktop
viewer into one run. Workflow success is not physical voice/screen, consumer-PC
or gaming acceptance. See [screen authorization](screen-authorization.md) for
specific cases and Groups 8/9's local consent/lifecycle requirements.

Graphify traced JWT sources, diagnostic capture consent, participant identities
and eviction/test dependencies at indexed commit `9b745bd` (before Group 6).
Current source and executable checks were reviewed independently; the graph is
navigation evidence, not runtime proof.

## Blockers before stable installer qualification

An alpha installer already exists from the owner's earlier request. Group 7
retains its lifecycle checks and shared payload. A stable installer still needs
desktop integration of screen authorization, publisher and viewer, everyday
settings/recovery, deployed physical two-PC media acceptance and measured gaming
qualification. It also needs complete dependency attribution, owner code signing,
clean consumer-PC startup and version-upgrade evidence. Neither another release
nor a deployment is included in this group. See the [completion roadmap](completion-roadmap.md).
