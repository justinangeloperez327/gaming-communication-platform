# Text messaging — Group 23

The Axum/PostgreSQL backend provides persistent **HTTP text-channel messages**. Every route requires an authenticated Bearer access token and checks current community membership in PostgreSQL. A user cannot access channels by guessing another community ID or by combining an authorized community with an unrelated channel ID.

There are no public unauthenticated message routes. Authenticated channel WebSocket events, typing and presence are documented in [realtime](realtime.md); friend-only direct messages in [friends/DMs](friends-dm.md). Reactions, attachments and the desktop message composer remain incomplete.

## API endpoints

All routes are relative to `/v1/communities/{community_id}/channels/{channel_id}/messages`.

| Method | Suffix | Authorization | Behavior |
| --- | --- | --- | --- |
| POST | / | Community member and **text** channel | Create message |
| GET | / | Member and text channel | Descending keyset pagination |
| GET | /{message_id} | Member and text channel | Read message or tombstone |
| PATCH | /{message_id} | **Message author**, still an active member | Edit own nondeleted message |
| DELETE | /{message_id} | **Author, community owner or admin** | Replace original text with tombstone |

### Message creation

```json
{"body":"Hello, party!"}
```

Messages require 1–4000 **UTF-8 bytes** of content and reject all-whitespace messages, NUL and inappropriate ASCII control characters. Newlines and tabs are permitted. Clients must still escape message content during rendering; storing plain text is not an HTML sanitization or moderation mechanism. An 8 KiB request body cap applies.

A successful creation returns HTTP 201 and a message object similar to:

```json
{
  "id":"00000000-0000-0000-0000-000000000001",
  "channel_id":"00000000-0000-0000-0000-000000000002",
  "author_id":"00000000-0000-0000-0000-000000000003",
  "body":"Hello, party!",
  "created_at_ms":1780000000000,
  "edited_at_ms":null,
  "deleted_at_ms":null
}
```

IDs/timestamps above are illustrative, not generated application records.

### Pagination

Use `GET /messages?limit=50` (default 50; 1–100 inclusive). Results are ordered newest-to-oldest by immutable `(created_at, id)`. When more records exist, the response contains the last returned UUID as `next_before`:

```json
{
  "messages": [],
  "next_before": null
}
```

Pass the returned cursor as `?limit=50&before=UUID` to retrieve the next page. The ID must be a valid UUID. A syntactically valid cursor belonging to another channel yields no further rows, never cross-channel data. For a busy channel, new inserts appear on newer pages; the keyset keeps older pagination stable.

### Edits and deletion

Only the original author can edit their own undeleted messages. Community owners/admins may delete anyone's message but **cannot rewrite another user's message**. Deletion is idempotency-protected: the first delete returns 204, subsequent attempts return 404.

To remove deleted content from the database's active message body, deletion overwrites it with the literal `[deleted]` marker and sets `deleted_at`. Reads always return `body:null` for these tombstones while preserving the message ID, timestamp, and author for history/pagination continuity. This is **not complete erasure from PostgreSQL WAL, replicas, backups or prior clients**, and retention/erasure policies remain to be implemented.

A missing or unauthorized community, wrong channel/community pair, voice channel, unknown message, or forbidden author-only mutation returns 404 without exposing private message data. Invalid input/cursor/limit returns 400. No database yields 503. Authentication failures yield 401.

## Tests and security boundaries

The `messages_access` PostgreSQL integration tests cover authenticated posting, anonymous denial, cross-community ID mixes, text-versus-voice channel isolation, input limits, author edits, manager deletes, deleted-content redaction in API and database, stable keyset pagination with no duplicate rows, and permission revocation after removing a member. Messages cannot be edited or deleted after tombstoning. These tests run alongside Groups 20–22 regression tests.

**Still outstanding before a public service:** abuse/spam rate limits, deletion audit records, retention/erasure policy, content moderation/reporting, WebSockets, read receipts, attachments, message-search and richer permission scopes. Tests do not certify production scaling, delivery guarantees, or gaming GPU performance.
