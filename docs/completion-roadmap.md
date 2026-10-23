# GCP completion roadmap

The goal is a lightweight Windows application usable by a private friend group.
The existing Qt desktop, Rust/PostgreSQL backend and LiveKit media architecture
remain. The alpha installer is an installation preview, not a completed service.
Each group needs reviewable code and evidence before its status is advanced.

| Order | Deliverable | Acceptance / current boundary |
| --- | --- | --- |
| 1 | Shared server package | Docker build, generated secrets, migrations, readiness/restarts, TLS configuration, persistence and backup/restore. This change implements the package; see [server stack](server-stack.md) and its CI results. No external deployment or physical media verification. |
| 2 | Account onboarding | Owner bootstrap, email-bound one-use invitations, desktop account creation/owner controls and actionable errors; see [onboarding](account-onboarding.md) and PR #80. Automated API/stack/Qt coverage; physical deployed-host acceptance and email/recovery remain outstanding. |
| 3 | Communities and channels | Desktop create/join/leave, channel CRUD/order, invitation issue/revoke and member administration with backend role checks; see [desktop communities](desktop-communities.md) and PR #81. Automated Qt/API/container coverage; no live directory updates, pagination or owner transfer. |
| 4 | Usable voice milestone | [PR #82](https://github.com/justinangeloperez327/gaming-communication-platform/pull/82): consent/rearming/access-recovery fixes and real loopback synthetic PCM transport, mute, isolation, explicit rejoin and active-peer eviction checks. See [voice evidence/acceptance](usable-voice.md). Owner-run two-PC audio/device/privacy/PTT/network acceptance remains pending. |
| 5 | Text messaging | [PR #83](https://github.com/justinangeloperez327/gaming-communication-platform/pull/83): desktop history/composer, author edits/manager deletes, safe explicit retries, live ID refetch and reconnect snapshots. See [desktop messaging](desktop-messaging.md); Qt/QML and real Qt→Rust→PostgreSQL checks. Physical two-PC/deployed network acceptance remains pending. |
| 6 | Friends and DMs | [PR #84](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84): desktop request/cancel/accept/decline, block/unblock and conversation flows, shared composer, safe retries and live private delivery. See [desktop friends](desktop-friends.md); Qt/QML, real Qt→Rust→PostgreSQL and private authorization/revocation tests. No physical two-PC/deployed-network acceptance. |
| 7 | Screen authorization design | Consent, identities, track ownership and voice/share coexistence enforced end to end. Current voice tokens allow microphone only. |
| 8 | Screen publisher | Desktop source picker, explicit consent, capture/encode/publish, stop/revoke and failure recovery. Existing publishers are diagnostics. |
| 9 | Screen viewer | Remote track subscription/rendering, visibility, resize/reconnect and cleanup in the desktop. |
| 10 | Everyday settings/recovery | Device persistence, onboarding/settings UX and network/session/device failure handling. |
| 11 | Gaming qualification | Measure CPU/GPU/RAM/frame-time cost on actual gaming hardware, then optimize measured bottlenecks without reducing media quality. |
| 12 | Distribution qualification | Complete dependency attribution, owner code signing, clean Windows and version-upgrade tests; stable package only after product acceptance. Alpha installation/repair/uninstall CI already exists. |

Only the shared host runs the backend, database and SFU. Friends use the desktop.
Host selection, domain/network access and authorization for an actual deployment
remain owner decisions. Physical two-user acceptance and publisher signing are
owner-run checks. Image uploads are deferred. No item in this roadmap authorizes
paid resources, deployment or another public release.
