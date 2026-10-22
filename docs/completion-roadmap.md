# GCP completion roadmap

The goal is a lightweight Windows application usable by a private friend group.
The existing Qt desktop, Rust/PostgreSQL backend and LiveKit media architecture
remain. The alpha installer is an installation preview, not a completed service.
Each group needs reviewable code and evidence before its status is advanced.

| Order | Deliverable | Acceptance / current boundary |
| --- | --- | --- |
| 1 | Shared server package | Docker build, generated secrets, migrations, readiness/restarts, TLS configuration, persistence and backup/restore. This change implements the package; see [server stack](server-stack.md) and its CI results. No external deployment or physical media verification. |
| 2 | Account onboarding | Owner bootstrap, email-bound one-use invitations, desktop account creation/owner controls and actionable errors; see [onboarding](account-onboarding.md) and PR #80. Automated API/stack/Qt coverage; physical deployed-host acceptance and email/recovery remain outstanding. |
| 3 | Communities and channels | Desktop create/join/invite/admin flows with backend role enforcement. Current desktop selects existing communities/voice channels. |
| 4 | Usable voice milestone | Real room integration/recovery plus owner-run two-PC audio, device, mute/PTT and consent acceptance. Current automated tests do not prove audibility. |
| 5 | Text messaging | Desktop history/composer, live events and reconnect recovery connected to existing authorized API. |
| 6 | Friends and DMs | Desktop request/block/conversation flows and live delivery using existing backend operations. |
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
