# Friends and Direct Messages — Group 25

Every endpoint requires a valid PostgreSQL-backed bearer token. Friend requests use an exact handle; DMs use canonical pairs of user UUIDs. **DM content is not end-to-end encrypted.** The group has no public user directory.

## API routes

| Method | Route | Purpose |
| --- | --- | --- |
| POST | `/v1/friends/requests` | Send `{"handle":"player_one"}` |
| GET | `/v1/friends/requests` | List up to 100 own pending requests |
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
| POST | `/v1/dms/{id}/messages` | Send `{"body":"private message"}` |
| PATCH | `/v1/dms/{id}/messages/{message_id}` | Author edits own message |
| DELETE | `/v1/dms/{id}/messages/{message_id}` | Author deletes own message |

A pending friend request cannot be duplicated in either direction. The recipient alone can accept or decline it, and accepted requests create a canonical friendship pair. Blocking automatically removes both friendship and pending requests. If either user blocks the other, *both* lose DM access, and the blocked person cannot send further friend requests. Unblocking does **not** automatically restore a friendship.

DM access requires current friendship and the absence of any block in either direction; knowing a conversation UUID does not grant access. Conversation creation is idempotent per pair. Messages are newest-first using `(created_at, id)` keyset pagination, with a maximum body of 4,000 UTF-8 bytes.

Deletion replaces active message text with `[deleted]` and returns `body:null` for historical tombstones. This does not erase backup/WAL copies or message content previously displayed to clients. The conversation remains stored after unfriend or block, but inaccessible until the users become friends again.

## Security and operational scope

Pairwise friend/block/DM-send transitions use deterministically ordered database user-row locks and SQL authorization predicates. Anonymous clients receive 401 and unrelated account/conversation lookups do not expose DM content.

No end-to-end encryption, real-time DM WebSocket delivery, read receipts, push notification, attachment uploads, distributed spam throttling, blocking enforcement inside shared community channels, or retention/erasure policy is implemented. This is an integration-test foundation rather than a public-internet production messaging service.

Integration tests exercise three separate registered users, request acceptance and replay, unauthorized DM attempts, edit/delete ownership, paging/tombstones and bilateral block behavior using the disposable PostgreSQL service in GitHub Actions.
