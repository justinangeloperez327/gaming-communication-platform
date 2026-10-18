# Architecture

GCP retains three separate runtime boundaries:

- **Qt/C++20 desktop:** in-memory account session, asynchronous authenticated HTTP requests, community/voice-channel selection, subprocess lifecycle and consent UI.
- **Rust/Axum modular monolith:** PostgreSQL-backed accounts/sessions, community roles, messages, friends/DMs, channel event sockets and restricted LiveKit voice JWT issuance.
- **Native LiveKit/Windows media:** a voice child process uses WebRTC platform audio. Separate diagnostic executables exercise Windows Graphics Capture, CPU readback or same-device D3D11 BGRA→NV12 conversion, asynchronous hardware-class Media Foundation H.264 output and encoded LiveKit transport.

LiveKit SFU media bypasses the Rust application API. PostgreSQL stores durable metadata and messages; there is no implemented Redis/object-storage dependency. Desktop messaging, stream viewing, screen-sharing controls and window selection remain incomplete. Synthetic video and pre-encoded files are explicit diagnostic inputs.

The desktop obtains fresh, microphone-only room grants from the API using current membership. GCP logout clears local state and stops local voice. Backend session/membership revocation prevents new tokens but does not evict an already connected SFU participant. Token expiry does not imply active-room expiry. The desktop now checks current channel/session access periodically and stops on failure; a separate LiveKit control-plane integration is still needed to enforce that boundary for arbitrary clients. See [long-session voice](long-session-voice.md).

Networking and queues are bounded; password hashing uses a bounded worker semaphore. Native capture owns GPU/COM resources through RAII and shared callback state; hardware-class encoding is event-driven. Physical driver behavior, remote media delivery and gaming resource usage are unverified.

Builds use CMake/Qt and Cargo. PR/manual CI and main-push CI validate the application, API and private portable ZIP without deployment or release. See [stabilization audit](stabilization-audit.md) for verified checks and remaining risks.
