# Desktop voice (experimental)

Build Windows with `GCP_ENABLE_LIVEKIT=ON`, placing `gcp-livekit-voice.exe`, `livekit.dll`, `livekit_ffi.dll` and Qt runtime dependencies alongside the desktop. Use the private ZIP tooling for dependency staging.

1. Configure PostgreSQL and server-only LiveKit credentials on the Rust backend.
2. Register accounts and create/join a community with a voice channel through the API; those administration screens are not implemented.
3. Open the desktop and enter the HTTPS API origin, account email and password. Loopback HTTP is allowed for development.
4. Choose your community and voice channel. Scan/select devices if needed.
5. Choose Listen only, or explicitly authorize microphone capture for this call and Join with mic. Leave stops the subprocess.

No desktop environment token configuration is required. The account controller rotates access/refresh tokens in memory; each join fetches a new authorized LiveKit token. Room credentials go to the native child's environment, not command-line arguments. This reduces accidental exposure but is not secure erasure of all Qt/SDK/environment memory copies.

The UI distinguishes running process, SDK connected acknowledgment, reconnecting, device loss and timeout. SDK connection does not establish microphone transmission, speaker audibility or remote quality. Selected device loss is polled; default-device rerouting is not fully controlled. Fresh microphone consent is required after a stopped call.

Sessions remain diagnostics capped at 300 seconds. There is no in-call mute UI, push-to-talk, participant mixer or uninterrupted long-call implementation. Sign-out stops local media and attempts API logout; offline logout can fail remotely. Active SFU eviction after revoked membership/session is not implemented. Physical two-user voice, microphone permissions, headset loss/recovery and gaming overhead require manual Windows acceptance.
