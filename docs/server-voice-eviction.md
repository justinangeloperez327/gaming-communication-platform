# Server-side voice access enforcement

The Rust server polls LiveKit active rooms and checks participants against current PostgreSQL sessions, voice channels and community membership. Logout, session expiry/deletion, membership removal and channel/community deletion therefore remove access without trusting the desktop to leave. Refreshing a login preserves its participant identity; separate logins remain independent.

## Operation

Use the existing `LIVEKIT_URL`, `LIVEKIT_API_KEY` and `LIVEKIT_API_SECRET`. The control API uses the same origin: WSS becomes HTTPS and loopback WS becomes HTTP. URLs with credentials, query strings, fragments or non-root paths are rejected. The server must reach LiveKit's `/twirp/livekit.RoomService/` endpoints with credentials authorized for room listing and participant administration. Redirects and ambient HTTP proxies are disabled so admin credentials stay on the configured origin. Admin JWTs expire after 30 seconds and never reach the desktop.

Reserve `gcp-voice-{canonical channel UUID}` rooms exclusively for this GCP deployment. Other rooms are untouched. Participants in reserved rooms must use `gcp-session-{canonical login session UUID}`; legacy user-only identities are removed and must rejoin with a new grant. Do not share this namespace between deployments.

The binary drives one reconciliation loop alongside the HTTP server. Embedders using `router_with_livekit` must also drive `run_eviction_loop`. Configuration begins unready: `/ready` and new voice grants return 503 until a complete pass succeeds. Any failed pass or a successful pass older than 30 seconds blocks new grants. Other API functions remain available.

The loop checks every five seconds when passes complete promptly. Each pass has a 20-second deadline; each HTTP request has a three-second timeout and responses are limited to 256 KiB. Limits are 64 total active rooms and 256 participants per room. Exceeding these limits blocks new grants. Rooms rotate across passes to reduce starvation from slow requests. Reconciliation uses one batched SQL authorization query per occupied GCP room and holds no database lock during HTTP calls. Failures retry on subsequent passes; restart recovers from actual room and database state without a volatile revocation queue.

## Limits and verification

Removal is periodic, not instantaneous. Database or SFU outages can delay removal; refusing new grants cannot disconnect participants while the SFU control API is unreachable. Authorization can change after a snapshot and will be checked again. Five seconds is not a guaranteed eviction deadline under load.

RemoveParticipant sends an explicit `revoke_token_ts` cutoff. LiveKit Cloud supports token revocation; self-hosted removal does not invalidate an already issued JWT. A modified client can temporarily rejoin with a cached valid token and is removed on a later pass. This is not a zero-gap denylist. Keep clocks synchronized and restrict service access to the intended private group. See LiveKit's [token revocation reference](https://docs.livekit.io/frontends/reference/tokens-grants/) and [RoomService API](https://docs.livekit.io/reference/other/roomservice-api/).

Tests use an HTTP LiveKit control fixture with real HS256 admin signatures and PostgreSQL. They cover two independent logins, refresh, logout, expired/deleted sessions, removed membership, deleted channels/communities, retry, restart, unauthorized rejoin, readiness, malformed/oversized responses and redirect rejection. They do not run a real SFU or prove physical two-user voice, screen sharing, or gaming performance. A real Windows/LiveKit test remains required before installer work.
