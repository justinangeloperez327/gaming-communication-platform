# Server-side voice access enforcement

The Rust server polls LiveKit active rooms and checks participants against current PostgreSQL sessions, voice channels and community membership. Logout, session expiry/deletion, membership removal and channel/community deletion therefore remove access without trusting the desktop to leave. Refreshing a login preserves its participant identity; separate logins remain independent.

## Operation

Use the existing `LIVEKIT_URL`, `LIVEKIT_API_KEY` and `LIVEKIT_API_SECRET`. The control API uses the same origin: WSS becomes HTTPS and loopback WS becomes HTTP. URLs with credentials, query strings, fragments or non-root paths are rejected. The server must reach LiveKit's `/twirp/livekit.RoomService/` endpoints with credentials authorized for room listing and participant administration. Redirects and ambient HTTP proxies are disabled so admin credentials stay on the configured origin. Admin JWTs expire after 30 seconds and never reach the desktop.

`LIVEKIT_CONTROL_URL` optionally selects a separate server-only HTTPS origin or
literal-loopback HTTP origin. It follows the same root-path and credential-free
URL restrictions and does not alter `LIVEKIT_URL` in issued desktop grants. See
the [single-host stack](server-stack.md) for its loopback control configuration.

Reserve `gcp-voice-{canonical channel UUID}` rooms exclusively for this GCP deployment. Other rooms are untouched. Participants in reserved rooms must use `gcp-session-{canonical login session UUID}`; legacy user-only identities are removed and must rejoin with a new grant. Do not share this namespace between deployments.

The binary drives one reconciliation loop alongside the HTTP server. Embedders using `router_with_livekit` must also drive `run_eviction_loop`. Configuration begins unready: `/ready` and new voice grants return 503 until a complete pass succeeds. Any failed pass or a successful pass older than 30 seconds blocks new grants. Other API functions remain available.

The loop checks every five seconds when passes complete promptly. Each pass has a 20-second deadline; each HTTP request has a three-second timeout and responses are limited to 256 KiB. Limits are 64 total active rooms and 256 participants per room. Exceeding these limits blocks new grants. Rooms rotate across passes to reduce starvation from slow requests. Reconciliation uses one batched SQL authorization query per occupied GCP room and holds no database lock during HTTP calls. Failures retry on subsequent passes; restart recovers from actual room and database state without a volatile revocation queue.

## Limits and verification

Removal is periodic, not instantaneous. Database or SFU outages can delay removal; refusing new grants cannot disconnect participants while the SFU control API is unreachable. Authorization can change after a snapshot and will be checked again. Five seconds is not a guaranteed eviction deadline under load.

RemoveParticipant sends an explicit `revoke_token_ts` cutoff. LiveKit Cloud supports token revocation; self-hosted removal does not invalidate an already issued JWT. A modified client can temporarily rejoin with a cached valid token and is removed on a later pass. This is not a zero-gap denylist. Keep clocks synchronized and restrict service access to the intended private group. See LiveKit's [token revocation reference](https://docs.livekit.io/frontends/reference/tokens-grants/) and [RoomService API](https://docs.livekit.io/reference/other/roomservice-api/).

Tests use an HTTP LiveKit control fixture with real HS256 admin signatures and PostgreSQL. They cover two independent logins, refresh, logout, expired/deleted sessions, removed membership, deleted channels/communities, retry, restart, unauthorized rejoin, readiness, malformed/oversized responses and redirect rejection. Those fixture tests do not run a real SFU; the dedicated contract below and stack acceptance do. Physical two-user voice, screen sharing and gaming performance remain unverified. The alpha installer is available, with those product acceptance gaps still open.

## Real control API compatibility check

CI also runs `python3 scripts/test_livekit_control_contract.py` on Linux x64 with Rust installed. It downloads the official LiveKit 1.13.10 server archive with a pinned SHA-256, starts it on loopback with disposable test credentials, runs one explicitly selected Rust contract test, and stops the process even on failure. It neither deploys a service nor needs production credentials. The regular Rust test suite ignores this external-server test; the dedicated CI step must pass it explicitly.

The contract checks real CreateRoom/ListRooms/ListParticipants responses, room-scoped admin authorization, rejection of insufficient/wrong-room grants and invalid signatures, missing-participant RemoveParticipant with the revocation cutoff, empty-room reconciliation/readiness, and DeleteRoom cleanup. There are no connected participants: successful execution does not verify removal of an active media peer, Cloud token revocation, self-hosted rejoin timing, TLS deployment, audio, screen sharing or gaming performance. PostgreSQL/mock-SFU tests continue to cover revocation state transitions separately.

## Active synthetic media peer contract

The separate [Windows SDK/SFU test](usable-voice.md) creates actual synthetic PCM
publisher/receiver peers and an isolated room. Before removal the runner checks
that the publisher has an active track, invokes real RoomService RemoveParticipant
and requires SDK disconnect/departure callbacks while the receiver remains
connected. This adds occupied-room control compatibility evidence. It does not
combine the Rust reconciliation loop, PostgreSQL revocation and physical users
in the same acceptance run, or establish cached-token rejoin timing.
