# Architecture

GCP retains three separate runtime boundaries:

- **Qt/C++20 desktop:** in-memory account session, asynchronous authenticated HTTP requests, community/channel/member/invitation and friend/block/conversation management, independent text/voice selection, bounded text history/composition, authenticated event sockets, subprocess lifecycle and consent UI.
- **Rust/Axum modular monolith:** PostgreSQL-backed accounts/sessions, community roles, messages, friends/DMs, channel event sockets and restricted LiveKit voice/screen JWT issuance with screen leases.
- **Native LiveKit/Windows media:** a voice child process uses WebRTC platform audio. Separate diagnostic executables exercise Windows Graphics Capture, CPU readback or same-device D3D11 BGRA→NV12 conversion, asynchronous hardware-class Media Foundation H.264 output and encoded LiveKit transport.

LiveKit SFU media bypasses the Rust application API. PostgreSQL stores durable metadata and messages; there is no implemented Redis/object-storage dependency. Stream viewing, screen-sharing controls and window selection remain incomplete. Synthetic video and pre-encoded files are explicit diagnostic inputs.

Account ownership is bootstrapped by an explicit local operator command. The
server enforces owner-only, email-bound invitations for new accounts; the Qt
account panel creates accounts and manages pending codes. Invitation consumption,
account creation and session issuance are one database transaction. Existing
community roles and media authorization remain independent of server ownership.
See [private onboarding](account-onboarding.md).

The desktop obtains fresh, microphone-only room grants from the API using current membership. GCP logout clears local state and stops local voice. The desktop checks current channel/session access periodically and immediately after SDK recovery, stopping on failure. An interruption clears microphone/PTT arming; Enable mic is required after reauthorization. The Rust server separately reconciles SFU participants against current database sessions and membership, removing unauthorized participants through LiveKit's control API. Removal is periodic and outages can delay it; a cached self-hosted JWT can rejoin until its expiry. Token expiry alone does not disconnect an established participant. See [long-session voice](long-session-voice.md) and [server-side eviction](server-voice-eviction.md).

The [shared host package](server-stack.md) composes the existing Rust service,
PostgreSQL and LiveKit on one Linux Docker Engine host. An explicit migration job
precedes the API, and readiness requires both database access and successful SFU
reconciliation. An optional Caddy proxy terminates HTTPS/WSS and restricts source
IPs. WebRTC media still goes directly to LiveKit. Database and certificate volumes
persist independently of containers; no service is deployed by adding this package.

Networking and queues are bounded; password hashing uses a bounded worker semaphore. Native capture owns GPU/COM resources through RAII and shared callback state; hardware-class encoding is event-driven. Physical driver behavior, remote media delivery and gaming resource usage are unverified.

The existing directory controller also owns community management. One bounded
request runs at a time; details load on demand. Refresh reconciles stable IDs
rather than clearing a valid room, and selection invalidation stops local voice.
Server-side SQL role predicates remain authoritative. Pending community codes are
revocable and limited per community without replacing the existing schema.

The text controller uses Qt WebSockets for identifier notifications and the
existing HTTP API for all durable operations. It subscribes before requesting a
current snapshot, replaces the recent window after a gap, and coalesces bounded
ID refetches. Client-generated message UUIDs reuse the existing database primary
key for safe explicit POST retries; no new table or broker is needed. Text
selection has its own signal and does not emit the voice selection-change
signal. See [desktop messaging](desktop-messaging.md).

Group 6 reuses that message model/controller/composer for private conversations.
A separate directory stages bounded friend/request/block/conversation refreshes;
its selection does not touch community or voice selection. DM notifications use
the same transport logic with a separate hub, preventing equal conversation and
channel UUIDs from sharing activity. Ordered pair locks serialize private writes
with block/unfriend, which wake active subscriptions only after commit. Current
friendship and bilateral block checks remain server-side; community roles never
grant DM moderation rights. See [desktop friends](desktop-friends.md).

Group 7 gives publishers and viewers distinct, short-lived database leases and
identities in separate `gcp-screen-{channel}` rooms. This is necessary because
LiveKit subscription grants apply to an entire room; voice must not implicitly
receive screen tracks. Publisher grants permit only screen video and no
subscription; viewer grants permit subscription and no publication. Current
access, lease purpose, track count/type and expiry are checked by reconciliation.
Stopping a lease does not revoke its login's voice identity. The authorized
publisher directory supplies identity/user mapping without trusting SFU metadata.
The upcoming desktop publisher/viewer must bind explicit local consent to a
source and channel, stop on interruption and consume these grants. See
[screen authorization](screen-authorization.md) for the API and implementation
contract; OS consent cannot be proved by a boolean sent to a server.

Builds use CMake/Qt and Cargo. PR/manual CI validates the application, API, real
disposable server stack, portable ZIP and per-user alpha installer. A separate
main-push job publishes the explicitly configured owner-requested preview once
after the required checks succeed; it skips a version already published. No
backend deployment is automated. See the [current stabilization audit](stabilization-status.md)
and [completion roadmap](completion-roadmap.md) for evidence and remaining risks.
