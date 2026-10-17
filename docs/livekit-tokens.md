# LiveKit Voice Room Authorization — Group 26

The Rust backend can issue a short-lived **LiveKit join JWT** for one voice channel to an authenticated user who is **currently a member of its community**. It does not expose the LiveKit signing secret to clients. This API is for the future native Windows voice experience; microphone capture/playback is implemented in Group 27.

The token structure uses LiveKit's documented HS256 `iss`, `sub`, `exp`, `nbf` and `video` grants. See [LiveKit's token and grants reference](https://docs.livekit.io/frontends/reference/tokens-grants/).

## Configuration

Set all three secrets/environment settings on the **Rust server**:

```powershell
$env:LIVEKIT_URL = "wss://voice.example.com"
$env:LIVEKIT_API_KEY = "YOUR_API_KEY"
$env:LIVEKIT_API_SECRET = "YOUR_RANDOM_LONG_SERVER_SECRET"
```

These must be set together. Partial configuration or invalid credentials causes startup to fail. An unconfigured server starts without LiveKit token issuance and responds with HTTP 503 to authorized issuance requests. The URL must be `wss://` for remote endpoints; `ws://127.0.0.1:7880` or `ws://localhost:7880` is permitted for loopback development only. The API secret is never logged or returned to a client.

All user account and community tables must already have been migrated using the controlled SQLx migration process. Bind the Rust server to loopback or protect it with an HTTPS reverse proxy and abuse throttling; public exposure has **not** been secured yet.

## Authorized endpoint

```http
POST /v1/communities/{community_id}/channels/{channel_id}/livekit-token
Authorization: Bearer ACCESS_TOKEN
```

Request body is empty. The backend verifies the login session using PostgreSQL, matches the channel with the supplied community, requires channel kind `voice`, and checks a current community membership. It derives a room name of the form `gcp-voice-{voice_channel_uuid}`; the client **cannot choose** an arbitrary room or identity.

A successful response is HTTP 201 and includes `Cache-Control: no-store`, `Pragma: no-cache`, and:

```json
{
  "server_url": "wss://voice.example.com",
  "participant_token": "<signed-join-JWT>",
  "room": "gcp-voice-00000000-0000-0000-0000-000000000001",
  "expires_in": 300
}
```

The participant identity is `gcp-session-{login_session_uuid}`. Separate logins have distinct identities; refreshing a login preserves its identity. LiveKit identity and room names can appear in server logs and traces; identifiers contain no email or real name.

A token has a five-minute join lifetime with an independent random JWT ID. It grants `roomJoin:true`, `canSubscribe:true`, `canPublish:true` restricted by `canPublishSources:["microphone"]`; it explicitly denies `canPublishData` and metadata self-updates. No room admin, record, ingress, screen-share or camera capabilities are granted. Microphone publishing on the SDK side must additionally respect app mute and the user's OS recording permissions.

## Security limits and revocation

Join-token expiry does not disconnect an already joined participant. The Rust server now reconciles active rooms against PostgreSQL and removes participants without current session and channel access. New grants and `/ready` remain unavailable until reconciliation succeeds. See [server-side voice eviction](server-voice-eviction.md) for configuration, operating limits, and self-hosted token-replay limitations.

Do not send the API secret to desktop clients or put credentials in URLs, analytics or application logs. The returned `participant_token` is itself sensitive and should be used only over HTTPS. This service has no distributed issuance rate limit or complete account abuse controls.

## Verification

The Rust unit test **verifies the real HS256 signature** and decodes the issued claims, including restricted media sources and token expiry. PostgreSQL integration tests create actual community membership, voice and text channels; test nonmember, wrong-community, wrong-channel-type, removed-member and revoked-session denial; ensure only a valid user can obtain a token with noncacheable headers. CI does not contact a LiveKit server, test microphone hardware, or confirm actual room join/playback.

The native C++ voice process connects to authorized rooms; see [desktop voice](desktop-voice.md). Physical Windows two-participant audio tests remain mandatory before claiming voice is working end to end.
