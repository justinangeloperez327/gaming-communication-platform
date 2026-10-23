# Screen authorization and consent — Group 7

Group 7 adds the backend authorization foundation for roadmap Groups 8 (desktop
publisher) and 9 (desktop viewer). It does **not** add a desktop Share button,
source picker, capture child protocol or rendered viewer. Existing monitor and
hardware publishers remain explicit operator diagnostics. A `consent:true`
request acknowledges intent; a server cannot prove that Windows displayed a
picker or that a modified client obtained consent.

## Separate media scopes

The existing C++/Qt → Rust/PostgreSQL → native LiveKit architecture remains.
Screen media uses the same SFU and a separate room because LiveKit
`canSubscribe` authorizes a whole room, without a per-source subscription grant.
Voice's automatic subscription must not silently subscribe to screen tracks.
No new service, SDK or runtime dependency is introduced.

| Purpose | Server-derived room | Server-derived participant | Publish | Subscribe |
| --- | --- | --- | --- | --- |
| Voice | `gcp-voice-{channel UUID}` | `gcp-session-{login UUID}` | Microphone only | Voice room |
| Screen publisher | `gcp-screen-{channel UUID}` | `gcp-screen-publish-{lease UUID}` | Screen video only | Denied |
| Screen viewer | `gcp-screen-{channel UUID}` | `gcp-screen-view-{lease UUID}` | Denied | Screen room |

All three require a current login, a current community membership, the correct
community/channel pair and a voice channel. A server-owner or community-owner
role does not bypass membership or grant control over another login's lease.
Screen access does not require an active microphone or a voice SFU participant.
The future desktop binds screen selection to the selected voice channel; the
API deliberately does not infer local capture consent from voice presence.
Shared-community blocking has the same limits as voice/text: a friend block
does not isolate people who remain members of that community.

JWT identities contain opaque IDs, never an email, source title or monitor name.
Clients cannot supply a room, participant identity, role outside `publish/view`
or signing key. Publishers receive `canPublishSources:["screen_share"]` and
`canSubscribe:false`; viewers receive `canPublish:false` and
`canSubscribe:true`. Both deny data publication and metadata self-updates and
receive no admin, recording, camera, microphone or screen-audio grant. Voice
tokens remain microphone-only with their existing five-minute join lifetime.
See LiveKit's [grant reference](https://docs.livekit.io/frontends/reference/tokens-grants/).

## API contract

All routes require `Authorization: Bearer ACCESS_TOKEN`; responses, including
errors, use `Cache-Control:no-store`. Successful grants also use
`Pragma:no-cache`. Only HTTPS is appropriate for a shared host; loopback HTTP is
for disposable development. API keys and secrets remain on the server.

The collection path is:

```text
/v1/communities/{community_id}/channels/{channel_id}/screen-sessions
```

| Operation | Request / result |
| --- | --- |
| `POST` collection | `{"purpose":"publish","consent":true}` or `view`; 201 with a new lease and grant |
| `POST {collection}/{lease_id}/renew` | `{"consent":true}`; 200 with a fresh JWT for the same active lease and identity |
| `DELETE {collection}/{lease_id}` | 204; removes only a lease owned by this login, then attempts an immediate SFU disconnect |
| `GET` collection | Current authorized publisher leases: `id`, `user_id`, `display_name`, `participant_identity`; no tokens or login IDs |

A grant contains `id`, `purpose`, `participant_identity`, `server_url`,
`participant_token`, `room` and `expires_in`. The join token and lease expire in
at most 60 seconds. The expiry is derived from the database lease; remaining
seconds may be shorter when work takes time. Renew requires the same login,
current access, a still-live lease and an explicit true acknowledgement. It
cannot revive an expired or stopped lease. Refreshing an account's bearer tokens
preserves the login and lease identity; old access credentials stop working.

Missing/false consent, unknown purpose and unexpected JSON fields are rejected
(400/422). Unauthenticated requests return 401; inaccessible scopes or another
login's renewal return 404. A second active lease of the same purpose returns
409, even for a different channel. Initial/failed/stale SFU reconciliation
blocks grant creation and renewal with 503. GET does not require healthy SFU
control; listed leases are authorization intent, **not proof of a live track**.

There is at most one publisher and one viewer lease per login. Creation is
serialized with renewal/account logout; uniqueness is enforced in PostgreSQL.
Channel/membership share locks serialize issuance with access removal. A request
rechecks session credentials and expiry after waiting for locks. Stop and renew
serialize on the lease row; after Stop commits, renew cannot resurrect it.
Transactions release all database locks before any SFU HTTP request.

Stop stays available after membership loss and when SFU health is unavailable.
Absent, repeated and other-login lease IDs all return 204 without revealing
ownership. 204 means database authorization was removed; it is not proof that
the remote SFU already disconnected a participant. A failed immediate disconnect
marks health unready; periodic reconciliation retries from actual SFU/DB state,
including after restart. The client must stop local capture **before** waiting
for HTTP. A lost create reply is an uncertain outcome: do not replay it in the
background. The orphan expires within the lease interval; an explicit retry can
return 409 until then. No durable outbox or automatic write replay is added.

The migration `20261023000000_screen_leases.sql` adds the lease table and channel
index. Rows cascade on channel/login deletion. Expired rows are replaced on the
next create for that login; at most two rows remain per login. Operators apply
the normal explicit migration before starting the updated API. Existing voice,
friend and message schemas and payloads remain compatible.

## Track ownership and revocation

Trust the authenticated publisher directory's lease/identity/user mapping.
Resolve an SFU track SID only under that exact participant and channel room;
do not trust a remote display name, track name, client-provided user ID or
metadata as proof of ownership. A matching lease may not have published yet.
An unknown, expired, wrong-channel or swapped-purpose identity is removed from
the reserved screen room. A voice identity cannot stand in for a screen lease,
or a screen identity for a voice session.

Reconciliation additionally permits zero or one screen-video track on a
publisher and zero tracks on a viewer. It accepts the documented numeric or
named SFU enums, rejecting unknown sources, non-video tracks and excess tracks.
This is periodic defense in depth; JWT source restrictions are the first gate.
Source labels describe the declared track, not its contents. A modified publisher
can lie about content, and an authorized viewer can record received pixels.
The application cannot prevent either by naming a track. No application E2EE,
remote-control channel or recording service is added.

The existing eviction loop now covers both reserved namespaces. It retains its
five-second scheduling, 20-second pass deadline, three-second HTTP timeout,
256 KiB response cap, 64 total active rooms and 256 participants per room. Voice
and screen rooms both count toward that room limit. Publisher listing refuses
more than 200 records instead of silently truncating ownership information.
Other SFU room namespaces remain untouched.

Stopping one screen lease leaves voice, the other lease purpose and other
logins alone. Logout, session expiry/deletion, membership loss and channel or
community deletion revoke all affected scopes. Removal is periodic and outages
can delay it. Token expiry alone does not stop an established connection, and
self-hosted LiveKit can accept a cached JWT again before expiry; reconciliation
removes the unauthorized participant on a later pass. This is not immediate or
gap-free revocation. See [server-side eviction](server-voice-eviction.md).

## Desktop implementation contract for Groups 8/9

These are requirements for the upcoming desktop work, not claims about a
controller or UI implemented by this group:

| Event | Required local behavior |
| --- | --- |
| Share requested | Capture remains stopped; show current community/channel and eligible source choices |
| Source chosen | Bind consent to the exact source handle, channel, account and a fresh local generation; show scope before confirmation |
| Picker cancelled / consent dismissed | Open no capture device, request no publisher grant, publish no track |
| Confirmation accepted | Request one publisher lease; validate its role/identity/room/origin and conservative local expiry before starting the selected source |
| Sharing active | Keep an obvious persistent indicator naming the chosen source and a reachable Stop control; keep a single bounded latest-frame queue |
| View requested | Require an explicit Watch action; acquire a viewer lease, disable automatic subscription and subscribe only to a directory-validated publisher/track |
| Lease renewal | Renew around every 20 seconds only while that local consent generation is valid; never extend the local deadline on a failed/late reply |
| Stop / close viewer | Stop local capture/rendering and disconnect the corresponding child first, clear tokens, invalidate generation, then revoke the lease |
| Source closed, permission denied, SDK interruption, child failure, logout, channel switch or access loss | Stop, revoke, clear source/consent and require a new explicit action; no fallback display or automatic resume |
| Mute / push-to-talk | Affect microphone transmission only; do not imply that screen sharing stopped |
| Stop share | Leave voice connected and its mute/PTT state unchanged |
| Text/friend selection | Keep the independent voice/share scope intact |

Only the trusted native capture path may consume the selected Windows source
handle. A consent flag alone must not start capture. Do not persist a source,
consent, JWT or captured thumbnail between launches. Use the existing sanitized
child environment/pipe pattern, with no bearer/JWT in command-line arguments,
URLs, diagnostics or crash telemetry. Local renewal uses a monotonic deadline
measured conservatively from the request start. If a native child cannot confirm
stop, terminate it before reporting success. Window selection, protected-content
behavior, device permissions, resize, reconnect and exit cleanup need dedicated
desktop tests in Groups 8/9.

## Evidence and limits

`apps/server/tests/livekit_eviction.rs` uses real PostgreSQL and an HTTP SFU
fixture. It verifies server JWT signatures and role/source restrictions,
consent rejection, correct membership/type/scope, concurrent create uniqueness,
login ownership, account rotation, publisher identity lookup, track limits,
stop/renew races, expiry, logout, bilateral namespace isolation, membership loss,
failed-stop retry and restart recovery. Existing voice and stack tests continue.

The Windows `gcp-livekit-screen-contract` runs the pinned C++ SDK against the
real disposable LiveKit server via `scripts/verify_voice_media.py`. It uses
synthetic pixels and fixture grants matching the separately tested Rust claims.
It attempts forbidden camera/microphone/viewer/voice-screen publication, verifies
decoded video and publish-only subscription denial, checks the actual owning
participant/track through RoomService, and removes that screen publisher while
synthetic voice continues decoding and unrelated screen peers remain connected.
The video fixture selects one software VP8 layer for deterministic authorization
checks; product encoder/quality defaults remain unchanged. The existing synthetic PCM
contract still runs. Neither contract opens a real screen or audio device.

The Rust lease API/DB tests and Windows SDK/SFU media contract are separate
executions. They do not establish a complete desktop→API→Windows capture→remote
viewer flow. Physical two-PC screen sharing, visual quality, gaming performance,
OS picker/permission UX and deployed-network recovery remain unverified. No
release, deployment, version bump or installer implementation is included.
