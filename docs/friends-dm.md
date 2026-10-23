# Friends and direct-message API

Every endpoint requires a valid PostgreSQL-backed bearer token. Friend requests use an exact handle; DMs use canonical pairs of user UUIDs. **DM content is not end-to-end encrypted.** The group has no public user directory.

## API routes

| Method | Route | Purpose |
| --- | --- | --- |
| POST | `/v1/friends/requests` | Send `{"handle":"player_one"}` |
| GET | `/v1/friends/requests` | List up to 100 own pending requests and the other user's handle/display name |
| DELETE | `/v1/friends/requests/{id}` | Sender cancels a pending request |
| POST | `/v1/friends/requests/{id}/accept` | Recipient accepts unblocked pending request |
| POST | `/v1/friends/requests/{id}/decline` | Recipient declines request |
| GET | `/v1/friends` | List up to 200 friends |
| DELETE | `/v1/friends/{user_id}` | Unfriend |
| POST | `/v1/blocks` | Block with `{"user_id":"UUID"}` |
| GET | `/v1/blocks` | List up to 200 blocked users |
| DELETE | `/v1/blocks/{user_id}` | Unblock (does not restore friendship) |
| GET | `/v1/dms` | List up to 100 accessible private conversations |
| POST | `/v1/dms` | Open/create friend conversation using `{"user_id":"UUID"}` |
| GET | `/v1/dms/{id}/messages` | History with `?limit=50&before=UUID` (limit 1–100) |
| POST | `/v1/dms/{id}/messages` | Send text, optionally with `client_message_id` |
| GET | `/v1/dms/{id}/messages/{message_id}` | Current authorized message/tombstone |
| GET | `/v1/dms/{id}/events` | Authenticated identifier-only WebSocket notifications |
| PATCH | `/v1/dms/{id}/messages/{message_id}` | Author edits own message |
| DELETE | `/v1/dms/{id}/messages/{message_id}` | Author deletes own message |

A pending friend request cannot be duplicated in either direction. The recipient alone can accept or decline it, and accepted requests create a canonical friendship pair. Blocking automatically removes both friendship and pending requests. If either user blocks the other, *both* lose DM access, and the blocked person cannot send further friend requests. Unblocking does **not** automatically restore a friendship.

DM access requires current friendship and the absence of any block in either direction; knowing a conversation UUID does not grant access. Conversation creation is idempotent per pair. Messages are newest-first using `(created_at, id)` keyset pagination, with a maximum body of 4,000 UTF-8 bytes.

Deletion replaces active message text with `[deleted]` and returns `body:null` for historical tombstones. This does not erase backup/WAL copies or message content previously displayed to clients. The conversation remains stored after unfriend or block, but inaccessible until the users become friends again.

## Security and operational scope

Pairwise friend/block/DM-create/edit/delete transitions use deterministically ordered database user-row locks and SQL authorization predicates. Anonymous clients receive 401 and unrelated account/conversation lookups do not expose DM content.

The [desktop friends/private client](desktop-friends.md) implements request,
block and conversation flows. The DM socket shares the bounded channel transport
implementation with a separate event hub and `conversation_id` field. It checks
current session, friendship and either-direction blocks at subscription, before
every forwarded event and periodically. Block/unfriend wakes existing subscribers
after commit. No body or token is included in notifications; GET retrieves content.

Optional non-nil `client_message_id` UUIDs make explicit POST retries safe using
the existing primary key. A retry by the same current author in the exact
conversation returns 200 with its current record, even if edited/deleted. Other
author/conversation collisions return 404; invalid/nil IDs return 400. New inserts
return 201 and publish once after commit. Older clients may omit the field.
`created_at_us` is an additive microsecond timestamp preserving full page order.

There is no application E2EE, durable/cross-process event delivery, read receipt,
push notification, attachment upload, distributed spam throttling, shared-channel
blocking enforcement or retention/erasure policy. The friend group needs the
existing private-network and HTTPS/WSS boundary.

Integration tests cover three registered users, request direction/cancellation, acceptance/replay, unauthorized HTTP/socket attempts, concurrent send IDs, edit/delete ownership and pair locks, paging/tombstones, bilateral block/unfriend and session revocation. Equal UUIDs in community and DM topics remain isolated. Separate actual Qt-to-Rust/PostgreSQL and shipped QML tests cover desktop behavior; see the [guide](desktop-friends.md).
