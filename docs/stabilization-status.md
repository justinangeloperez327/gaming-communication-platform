# Current stabilization audit — Group 6

GCP remains an experimental private Windows communication application. Groups
1–5 are merged. [Group 6 / PR #84](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84)
connects desktop friends and private conversations to the authorized API and
adds private event delivery using the existing message/controller architecture.
The current unsigned alpha installer remains an installation preview.
This group changes no release version and includes no merge, service deployment,
public release or installer implementation.

## Completed fixes

| Area | Implemented | Evidence and limits |
| --- | --- | --- |
| Shared server | Generated secrets, migrations, readiness/restarts, restricted TLS configuration, persistence and backup/restore | Merged PR #79; real-container acceptance, no external deployment |
| Accounts | Local stdin owner bootstrap, hashed email-bound one-use invitations, atomic invited signup and desktop owner controls | Merged PR #80; PostgreSQL/Qt/container tests; no email delivery, recovery, MFA or persistent login |
| Communities | Desktop create/join/leave, channel CRUD/order, invitations/member roles and selection-preserving refresh | Merged PR #81; API/Qt coverage; capped lists, no live directory updates or owner transfer |
| Voice | Per-call consent, muted/PTT-disarmed interruption recovery, immediate current-access recheck, bounded token-rotation recovery and native cleanup | Merged PR #82; Qt/HTTP/QProcess tests and real SDK/SFU synthetic PCM, mute/isolation/rejoin/active-eviction contract; owner two-PC/device acceptance remains pending |
| Desktop text | Separate text selection, chronological paginated history, Unicode plain-text composer, author edit, role-aware confirmed deletion, member names and live ID refetch | Merged PR #83; shipped QML/HTTP/WS tests and separate real Qt→Rust→PostgreSQL acceptance; no physical friend-user claim |
| Send integrity | Optional client message UUID reuses the existing primary key; explicit retries return the current authorized record and cannot overwrite edits/tombstones or disclose another author's collision | Concurrent PostgreSQL tests, ambiguous-response Qt retry fixture; no new schema or application runtime service |
| History integrity | Microsecond timestamps preserve ordering; gaps refetch recent history, events coalesce, rotation replaces auth headers, stale replies abort; missing IDs from uncommitted sends cannot cause repeated resync | Qt loss/rotation/pagination/revocation and request-loop tests; socket fanout remains single process, no durable replay |
| Friends/private messages | Desktop request/cancel/accept/decline, block/unblock and conversation flows; shared composer and bounded live DM/history recovery | PR #84; real Qt→Rust→PostgreSQL two-account flows and shipped FriendsPanel tests; physical/deployed acceptance remains pending |
| Private authorization | All DM writes use the block/unfriend pair locks; current friendship/bilateral block and author-only checks, post-commit revocation wakeups and isolated event hubs | Three-account HTTP/socket tests include equal channel/conversation UUIDs, concurrent retry IDs, tombstones, outsider denial and session logout |
| Resource use | Virtualized history capped at 200 records, ten memory-only drafts, 256 queued event IDs, one read/one mutation, bounded replies/timeouts and reconnect backoff | Explicit limits and regression tests; no measured gaming resource reduction or exhaustive leak-free claim |
| Distribution | Qt WebSockets included in the Windows SDK install and required in the bundle manifest; missing-DLL rejection added | Existing ZIP, notices, signature inventory and alpha install/repair/uninstall checks retained; full attribution/signing/clean-PC qualification remain open |

The C++20/Qt, Rust/Axum/PostgreSQL and native LiveKit architecture is preserved.
Qt WebSockets was added in Group 5. Group 6 adds no runtime dependency,
broker or database migration; it reuses message transport/UI and server event
handling instead of duplicating them. Media quality defaults and screen
diagnostics are unchanged.

## Verified behavior versus unfinished functionality

| Subsystem | Automated execution | Remaining work or acceptance |
| --- | --- | --- |
| Accounts/community | Real database/HTTP/container onboarding and role checks plus offscreen desktop controls | Deployed-host acceptance, recovery/email, owner transfer, live directory updates and pagination |
| Voice controls/transport | Actual Qt controls with controlled native child; separate real SDK/SFU synthetic PCM contract | Complete deployed desktop→API→PlatformAudio→friend speakers; physical privacy/audibility/F8/default devices, actual SDK network interruption and long gaming calls |
| Text | Shipped QML/controller protocol tests, real two-account Qt/Rust/PostgreSQL integration, concurrent DB retry and authorization tests | Physical two-PC/deployed network acceptance; search/attachments/reactions/typing UI/durable outbox; cross-server events |
| Friends/DMs | Actual desktop/controller request, block and private conversation flows; real authorized HTTP/socket delivery and bilateral revocation | Physical/deployed acceptance; push/unread/attachments/search and cross-server event propagation remain absent |
| Screen sharing | Consent-gated capture/encoder/transport diagnostics and parser/format tests | Desktop consent/source picker/publisher/viewer and track authorization; physical remote decode/quality: Groups 7–9 |
| Windows distribution | Hosted build, staged/extracted/installed runtime and dependency integrity checks | Consumer Windows PC, version upgrades, complete transitive licensing/source obligations and owner publisher signing |
| Performance | Telemetry/export tools and fixture validation | Matched actual gaming CPU/GPU/RAM/frame-time measurements; no measured budgets yet |

## Security and consent boundaries

Protected operations use current database sessions and membership/friendship.
Message edits remain author-only. Community owners/admins can delete channel
messages; only an author can delete a DM. The channel/private clients send bearer
headers to the approved origin and clear visible history/affected drafts after
confirmed access loss. Messages are plain text to
the server/database, without application E2EE. Backend credentials remain
server-side; no token belongs in an event, URL, log or native child command line.

Registration requires owner-issued account invitations. Email ownership,
recovery, MFA and complete public-internet abuse controls are absent. The
intended friend group needs a private network boundary and HTTPS/WSS. Channel
event fanout remains single process; there is no durable replay or cross-node
presence. A reconnect restores current recent pages and can discard an older
browsing window. Drafts exist only in memory, and Retry is an explicit action.

Microphone capture still requires per-call consent, muted recovery and explicit
rearming. Mute/PTT controls transmission; the device may stay open until Leave.
The cooperative desktop checks and server's periodic SFU reconciliation have
separate limits; outages can delay eviction, and cached JWTs can rejoin before
expiry. Voice grants allow microphone publication only. Screen diagnostics use
separate explicit consent/operator credentials; they do not constitute desktop
sharing authorization. See [voice acceptance](usable-voice.md).

Package hashes prove consistency, not publisher identity. The notice catalog is
a baseline, not complete attribution. No stable-distribution approval is implied.

## Checks and evidence

The [PR #84 checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84/checks)
record the exact tested head. Required checks include Rust formatting/Clippy and
real PostgreSQL tests, sanitizer/Python suites, Linux LiveKit control and real
container acceptance, Windows Qt/native build with 24 CTests, real synthetic PCM,
ZIP/package/installed runtime checks, and real Qt→Rust channel/friend/DM
integration. The new complete-window startup test catches required QML binding
failures before packaging.
The branch stays draft until these checks pass. Local Python tests (49) and
`git diff --check` passed; Rust/Qt execution uses CI because this editing host lacks
those toolchains. Workflow success is not physical voice, screen, clean-PC or
gaming acceptance.

Graphify was used to trace friend/block/DM authorization and test dependencies.
Its index predates Group 5, so current Qt sources and executable checks were
reviewed directly; the graph is navigation evidence, not runtime proof. The
actual changed sources and executable checks establish the implementation claims.
See [desktop friends](desktop-friends.md) and [channel messaging](desktop-messaging.md)
for behavior, resource bounds and specific test coverage, and
[completion roadmap](completion-roadmap.md) for the remaining groups.

## Blockers before stable installer qualification

An alpha installer already exists from the owner's earlier request. Group 6
retains its lifecycle checks and shared payload without implementing a new one.
A stable installer still needs complete desktop screen authorization, publisher
and viewer, everyday settings/recovery, deployed physical two-PC media acceptance
and measured gaming qualification. It also
needs complete dependency attribution, owner code signing, clean consumer-PC
startup and version-upgrade evidence. Neither another release nor a deployment
is part of this group.
