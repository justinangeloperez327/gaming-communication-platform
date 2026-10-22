# Architecture

GCP retains three separate runtime boundaries:

- **Qt/C++20 desktop:** in-memory account session, asynchronous authenticated HTTP requests, community/voice-channel selection, subprocess lifecycle and consent UI.
- **Rust/Axum modular monolith:** PostgreSQL-backed accounts/sessions, community roles, messages, friends/DMs, channel event sockets and restricted LiveKit voice JWT issuance.
- **Native LiveKit/Windows media:** a voice child process uses WebRTC platform audio. Separate diagnostic executables exercise Windows Graphics Capture, CPU readback or same-device D3D11 BGRA→NV12 conversion, asynchronous hardware-class Media Foundation H.264 output and encoded LiveKit transport.

LiveKit SFU media bypasses the Rust application API. PostgreSQL stores durable metadata and messages; there is no implemented Redis/object-storage dependency. Desktop messaging, stream viewing, screen-sharing controls and window selection remain incomplete. Synthetic video and pre-encoded files are explicit diagnostic inputs.

Account ownership is bootstrapped by an explicit local operator command. The
server enforces owner-only, email-bound invitations for new accounts; the Qt
account panel creates accounts and manages pending codes. Invitation consumption,
account creation and session issuance are one database transaction. Existing
community roles and media authorization remain independent of server ownership.
See [private onboarding](account-onboarding.md).

The desktop obtains fresh, microphone-only room grants from the API using current membership. GCP logout clears local state and stops local voice. The desktop checks current channel/session access periodically and stops on failure. The Rust server separately reconciles SFU participants against current database sessions and membership, removing unauthorized participants through LiveKit's control API. Removal is periodic and outages can delay it; a cached self-hosted JWT can rejoin until its expiry. Token expiry alone does not disconnect an established participant. See [long-session voice](long-session-voice.md) and [server-side eviction](server-voice-eviction.md).

The [shared host package](server-stack.md) composes the existing Rust service,
PostgreSQL and LiveKit on one Linux Docker Engine host. An explicit migration job
precedes the API, and readiness requires both database access and successful SFU
reconciliation. An optional Caddy proxy terminates HTTPS/WSS and restricts source
IPs. WebRTC media still goes directly to LiveKit. Database and certificate volumes
persist independently of containers; no service is deployed by adding this package.

Networking and queues are bounded; password hashing uses a bounded worker semaphore. Native capture owns GPU/COM resources through RAII and shared callback state; hardware-class encoding is event-driven. Physical driver behavior, remote media delivery and gaming resource usage are unverified.

Builds use CMake/Qt and Cargo. PR/manual CI validates the application, API, real
disposable server stack, portable ZIP and per-user alpha installer. A separate
main-push job publishes the explicitly configured owner-requested preview once
after the required checks succeed; it skips a version already published. No
backend deployment is automated. See the [current stabilization audit](stabilization-status.md)
and [completion roadmap](completion-roadmap.md) for evidence and remaining risks.
