# Real-time channel events and ephemeral presence — Group 24

The Rust server supports a **single-server WebSocket event stream** for authenticated text-channel members. The WebSocket does **not** accept message creation, editing or deletion commands; all durable writes continue to use the Group 23, PostgreSQL-authorized HTTP message routes. Message bodies are intentionally excluded from WebSocket notifications.

## Connection and authentication

A native client can open:

`GET /v1/communities/{community_id}/channels/{channel_id}/events`

with `Authorization: Bearer ACCESS_TOKEN` and normal WebSocket Upgrade headers. The server authenticates against the PostgreSQL session table and verifies current membership of the exact requested **text** channel before upgrading. Anonymous, expired-session or nonmember subscriptions are rejected. Do not place access tokens in query strings or log them.

A browser's native `WebSocket` constructor cannot supply a custom Authorization header; this endpoint is used by the Qt/Windows text client. A secure browser-specific authentication scheme is not included.

Each connection receives a `ready` JSON event and subscribes to the server-local event bus. Other notification frames have this shape:

```json
{
  "type": "message_created",
  "channel_id": "00000000-0000-0000-0000-000000000001",
  "user_id": "00000000-0000-0000-0000-000000000002",
  "message_id": "00000000-0000-0000-0000-000000000003",
  "active": null
}
```

Message event types are `message_created`, `message_edited`, `message_deleted`. They are emitted **after** the authorized database change succeeds. The receiving client must fetch message content using the existing permission-checked `GET .../messages/{id}` endpoint; even a guessed message ID does not bypass membership.

## Presence and typing

Opening the first connection for a user in a channel emits `presence_joined`; closing the final connection emits `presence_left`. Presence is **ephemeral and process-local**. No permanent online/offline DB status is asserted, and an abrupt process crash can lose leave events.

The server accepts this sole client command:

```json
{"type":"typing","active":true}
```

The server sends a `typing` event with the authenticated sender's user ID and `active` value; one outbound typing event is permitted per connection every two seconds. No message content is accepted over the socket. For clients, typing status should expire locally if it stops updating; no durable typing state is stored.

## Bounded resource use and session revocation

- Maximum 200 WebSocket connections per text channel and four per user/channel per server process.
- Each subscribed channel has its own broadcast ring buffer of 256 identifier-only events. Traffic in other channels does not consume that buffer. The last subscription dropping removes the channel queue immediately; publishing without subscribers creates no queue or retained history. A lagged client receives `resync_required` and is disconnected; events are **not** silently replayed.
- Outbound write deadline: three seconds. Incoming frames are restricted to 2 KiB.
- Every forwarded event and a periodic ten-second check reverify **both** the PostgreSQL login session and current channel membership. A removed member or logged-out/expired session is disconnected.
- A ping occurs during periodic checks; no client activity for 75 seconds closes the session.
- No token is embedded in events, query strings or persistence.

### Reconnect and recovery

After subscribing or reconnecting, clients must refetch current HTTP history to reconcile missed message IDs, edits and deletions. The desktop requests the latest 50 messages and replaces its old window; users can load older pages again. On `resync_required` they must do the same. The WebSocket stream is an optimization for notification latency, not a durable event history. A client that needs edits/deletions while disconnected may have to refetch displayed message IDs and current pages.

### Important deployment limitation

**Fanout and presence are local to one Rust server process.** A message written to instance A does not currently notify WebSocket clients connected to instance B. Load-balanced deployments must use sticky routing as an interim workaround (which still does not guarantee cross-node events), or add PostgreSQL `LISTEN/NOTIFY` or an event broker/outbox before horizontally scaling. No exactly-once delivery or cross-node presence is claimed.

The [desktop client](desktop-messaging.md) implements bounded reconnect/backoff, a fresh history snapshot after each subscription, ID coalescing and HTTP polling while the socket is unavailable. Distributed event propagation, durable reconciliation/sequence numbers and complete rate limiting remain future work. LiveKit media and grants are separate from text events.

## Automated acceptance

`apps/server/tests/realtime_socket.rs` uses an actual loopback WebSocket server and temporary PostgreSQL user sessions to exercise authentication denial, nonmember denial, authorized subscription, message create/edit/delete notification, identifier-only payloads, typing, removal of a member while connected, and denial of subsequent connections. Remaining physical-device tests are unrelated to the backend event transport.

Queue regression tests flood one channel while another subscriber remains unread, verify that only the flooded channel lags, confirm cleanup after the last listener drops (including concurrent drops), and check that a new subscription has no historical replay. These are deterministic in-process queue tests; the PostgreSQL/WebSocket integration tests continue to validate authorization and event delivery. There is no global connection cap or cross-instance event transport.
