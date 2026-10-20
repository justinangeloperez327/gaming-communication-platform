# Desktop voice (experimental)

Build Windows with `GCP_ENABLE_LIVEKIT=ON`, placing `gcp-livekit-voice.exe`, `livekit.dll`, `livekit_ffi.dll` and Qt runtime dependencies alongside the desktop. Use the private ZIP tooling for dependency staging.

1. Configure PostgreSQL and server-only LiveKit credentials on the Rust backend.
2. Register accounts and create/join a community with a voice channel through the API; those administration screens are not implemented.
3. Open the desktop and enter the HTTPS API origin, account email and password. Loopback HTTP is allowed for development.
4. Choose your community and voice channel. Scan/select devices if needed.
5. Choose Listen only, or explicitly authorize microphone capture for this call and Join with mic. Leave stops the subprocess.

No desktop environment token configuration is required. The account controller rotates access/refresh tokens in memory; each join fetches a new authorized LiveKit token. Room credentials go to the native child's environment, not command-line arguments. This reduces accidental exposure but is not secure erasure of all Qt/SDK/environment memory copies.

The UI distinguishes running process, SDK connected acknowledgment, reconnecting, device loss and timeout. SDK connection does not establish microphone transmission, speaker audibility or remote quality. Selected device loss is polled; default-device rerouting is not fully controlled. Fresh microphone consent is required after a stopped call.

Desktop calls now continue until Leave using a leased native control pipe and periodic backend authorization. Microphone calls start muted; Enable mic permits transmission, and the optional F8 push-to-talk mode works outside the Qt window. Switching modes mutes the call and requires Enable mic again. Standalone diagnostics keep their bounded duration defaults. Read [long-session voice](long-session-voice.md) for control safety, token/reconnect boundaries and tests.

Sign-out stops local media and attempts API logout; offline logout can fail remotely. Active SFU eviction after revoked membership/session is not implemented; periodic checks only constrain this cooperative desktop client. Physical two-user voice, permissions, headset behavior, gaming PTT and multi-hour calls still need manual Windows acceptance.

### Device-scan output limits

The Qt scanner consumes stdout incrementally, retains at most 64 KiB of device-list output and terminates an oversized scan. Unused stderr goes directly to the OS null device. Failed scans preserve the previous device list and selection; the next scan starts with a fresh buffer. Offline subprocess fixtures cover oversized output, retry and a 4 MiB stderr stream. This bounds retained application scan output; it is not a measured whole-process memory budget or hardware device acceptance result.

### Native child environment

Before every voice or device-scan launch, the desktop removes `DATABASE_URL`, `LIVEKIT_API_KEY`, `LIVEKIT_API_SECRET`, `GCP_ACCESS_TOKEN`, `GCP_REFRESH_TOKEN`, and inherited `LIVEKIT_URL`/`LIVEKIT_TOKEN` variables using case-insensitive matching. A voice launch then supplies only its newly authorized LiveKit URL and room token; device scans receive neither. OS/runtime environment settings remain available and the parent environment is unchanged. Stored QProcess launch environments are cleared after startup, completion or failed startup.

Offline subprocess tests inject dummy credentials, including a mixed-case signing-secret name, and verify the actual scan/voice child environment, replacement room credentials and preserved runtime settings. This is filtering of known GCP credential variables, not a sandbox or an arbitrary-secret allowlist. Environment strings and SDK allocations are not guaranteed to be securely zeroized, and this does not protect against an administrator inspecting process memory.
