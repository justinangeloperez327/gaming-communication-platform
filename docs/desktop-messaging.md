# Desktop text messaging — Group 5

[PR #83](https://github.com/justinangeloperez327/gaming-communication-platform/pull/83)
connects the Qt desktop to the existing Rust/PostgreSQL text-message API and
identifier-only channel WebSocket. It preserves the native LiveKit voice path.

## Use

Sign in, choose a community, then choose a **text** channel. Channel lists never
select a room automatically. The separate voice selection stays connected while
text channels change. Community changes and logout retain the existing voice
shutdown behavior.

History loads the latest 50 messages, displayed oldest first. **Older messages**
loads earlier pages; the visible model holds at most 200 records. When browsing
older history, incoming messages outside that window show **Back to latest**
instead of moving the window. Reconnect or Refresh replaces the window with
current recent history so missed edits and deleted markers are reconciled.

The composer sends plain text with a 4000 **UTF-8 byte** limit. It displays the
byte count; nonempty Unicode text, line breaks and tabs are supported. Formatting
markup is displayed literally. There are no attachments, rich text, link previews,
read receipts, search, typing indicators or presence UI.

Authors may edit their undeleted messages. Authors and community owners/admins
may delete messages after confirmation; deletion displays a tombstone. Owner/admin
roles never allow rewriting another author's text. Rust checks current membership
and the applicable role/author predicate for every operation.

## Delivery and recovery

Each send creates a random UUID and includes it as `client_message_id`. If the
write commits but its response is lost or unreadable, the draft is retained and
**Retry send** reuses that ID. The existing database primary key prevents another
row. The server returns the current authorized record, including a later edit or
tombstone, rather than overwriting it. An ID belonging to another author or
channel returns 404. Clients omitting the optional ID retain the original API
behavior; they do not receive duplicate prevention on a new POST.

A draft with an uncertain result is frozen until Retry or **Clear draft**. Clear
cancels the retry, not a potentially committed server write. Check history before
composing a replacement. There is no automatic application-level mutation retry
or durable offline outbox. Up to ten channels retain one bounded draft each in
memory; an eleventh draft is refused instead of silently evicting another.
Changing channel cancels old requests. Returning restores its draft; logout,
confirmed community access loss and removal of the selected channel clear the
corresponding state. Closing the application loses drafts.

The socket subscribes first, then requests authoritative HTTP history. Message
notifications contain IDs only; content is fetched through the authorized GET
route. Disconnect, an invalid frame or `resync_required` causes bounded exponential
reconnect (one second initially, up to 30 seconds), resubscription and a fresh
snapshot. The client sends a heartbeat and polls HTTP every 30 seconds while live
updates are unavailable. Manual Refresh is also available. Access-token rotation
replaces the socket header and rechecks history without repeating writes.

Only one history/member/message read and one mutation are in flight per chat
controller. ID notifications coalesce in a set capped at 256; overflow requests
a new snapshot. Page/member replies are capped at 512 KiB, individual/mutation
replies at 16 KiB, socket frames/messages at 2 KiB, and requests/handshakes at ten
seconds. The ListView creates visible delegates rather than one widget per stored
message. No measured gaming CPU/GPU/RAM improvement is claimed.

## Security and deployment limits

Bearer tokens use HTTP/WS Authorization headers, never URLs. The client uses the
selected HTTPS/WSS origin (HTTP/WS only for localhost/127.0.0.1), rejects redirects,
and does not bypass TLS validation. Responses are validated for channel, IDs,
body limits, timestamps and page ordering before changing history. Generation
checks and request cancellation keep stale results out of a newly selected room.
A confirmed authorization loss removes visible history and affected drafts.

The backend's event fanout remains **single process**, without durable replay or
cross-instance delivery. Reconnect restores a current 50-message window, not an
unbounded transcript. Older pages can be loaded again. Messages are plaintext to
the API/database; application end-to-end encryption, moderation and complete
internet-facing abuse limits remain absent. Do not treat an owner/admin client
UI as the security boundary. See [messages](messages.md) and [events](realtime.md).

## Automated evidence

- `desktop_text_messaging` runs the actual shipped QML and controllers against
  loopback HTTP/WebSocket fixtures: pagination/microsecond ordering, bounded
  history, literal Unicode text, edits, confirmed deletes, uncertain send/retry,
  socket gaps, stale-channel cancellation, malformed/oversized replies,
  membership loss, logout and the real account refresh timer. Refresh after an
  uncommitted send cannot loop between a missing-ID lookup and another snapshot.
- `desktop_voice_control_and_reauthorization` selects a text channel while the
  controlled voice child remains connected, without another voice grant.
- `messages_access` exercises actual PostgreSQL authorization and concurrent
  duplicate POSTs, changed-content retries, tombstones, author/channel collisions
  and invalid/nil client IDs.
- **Qt to Rust messaging acceptance** builds the real desktop controllers with
  Qt Core/Network/WebSockets, runs a real Rust API against a dedicated disposable
  PostgreSQL database, and exercises two accounts, invitations, channels,
  bidirectional delivery, edits, manager deletion, resubscribe history and removal
  of a member. This is a local CI integration, not two physical Windows users.
- Windows CI builds Qt 6.8.3 WebSockets, requires `Qt6WebSockets.dll` in the
  manifested bundle, rejects a package missing it, and runs staged/extracted and
  existing installed runtime checks.

The [PR checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/83/checks)
identify exact tested heads and outcomes. Physical two-PC messaging, deployed
HTTPS/proxy/network behavior, audio devices, screen sharing and gaming performance
remain unverified. Group 6 adds the desktop friends/DM flows; those APIs are still
separate from this text-channel UI.
