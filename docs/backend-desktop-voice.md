# Group 29 — Just-in-time authenticated room credentials (developer integration)

Qt desktop now sends `POST /v1/communities/{community_id}/channels/{channel_id}/livekit-token` to the existing Rust API before starting the native voice executable. The backend validates the bearer session, current community membership and voice channel, then returns a five-minute LiveKit JWT. Desktop never requires `LIVEKIT_TOKEN` supplied manually. Only the child process receives room credentials, through its environment (no CLI arguments).

## Developer setup

Configure the Rust server and PostgreSQL according to the existing backend runbooks. Start the desktop with the following process environment:

- `GCP_API_URL`: HTTPS server origin only (HTTP supported solely for localhost/127.0.0.1).
- `GCP_ACCESS_TOKEN`: active opaque 64-character lowercase hexadecimal bearer access token from an existing signed-in account. Its backend validity is checked each join.
- `GCP_COMMUNITY_ID`: UUID for the community the signed-in account belongs to.
- `GCP_VOICE_CHANNEL_ID`: UUID of a voice channel in that community.

Do **not** put tokens in command-line arguments, shell history, screenshots, committed .env files, or logs. For a production client, these variables must be replaced with in-memory sign-in state and channel selection. Run the Windows Qt desktop with native LiveKit executable and DLLs next to it (see [native voice](native-voice.md)).

## Security behavior

- Only HTTPS or loopback HTTP origins, no URL paths, userinfo, query strings or fragments. HTTP redirects disabled. Ten-second network timeout.
- Backend authorization is repeated for every join. Unexpected status/body is rejected; credentials are not printed. Pending request is aborted on Leave.
- Native process limits sessions to up to 300 seconds. It is not a proof of joined/receiving LiveKit media.
- Credentials are placed into the child process environment; desktop or native process memory may retain copies until cleanup/termination. Host processes and OS admins could inspect memory/environment. Do not interpret this as secure operating-system credential storage.
- A revoked backend session/membership blocks future issuance but **does not eject existing participants from the SFU**. Backend-driven active participant removal remains necessary.
- Microphone consent is still explicit; no automatic publishing on listen-only.

## Remaining production work

Desktop sign-in/login + refresh, persistent account state and channels, user-selectable rooms, verified connected state callbacks, real audio/device tests, active SFU revocation, reconnection policies and process-memory/credential lifecycle hardening. This increment is an authenticated API bridge, not a production login implementation.
