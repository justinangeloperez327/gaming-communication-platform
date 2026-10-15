# Communities, channels and permissions (Group 22)

Only authenticated PostgreSQL-backed sessions may access communities. Every community has one immutable owner (until owner deletion removes the community). Owner, admin and member roles govern management capabilities. Authorized text message endpoints are documented in [messages](messages.md).

## API routes

| Method | Route | Permissions |
| --- | --- | --- |
| POST | `/v1/communities` | Any signed-in user; creates community plus owner membership in one transaction |
| GET | `/v1/communities` | Signed-in user; lists first 100 of their own communities |
| GET | `/v1/communities/{id}` | Community member |
| PATCH | `/v1/communities/{id}` | Owner or admin; rename |
| DELETE | `/v1/communities/{id}` | Owner only, cascades channels and memberships |
| GET | `/v1/communities/{id}/members` | Member; limited to 200 rows |
| PATCH | `/v1/communities/{id}/members/{userId}` | Owner only; assign admin/member, never owner |
| DELETE | `/v1/communities/{id}/members/{userId}` | Owner can remove non-owner; admin can remove members only |
| POST | `/v1/communities/{id}/invitations` | Owner/admin creates a one-use 24-hour invite |
| POST | `/v1/invitations/accept` | Any signed-in, nonmember user with valid unused invite token |
| GET | `/v1/communities/{id}/channels` | Member; limited to 200 rows |
| POST | `/v1/communities/{id}/channels` | Owner/admin; create text or voice channel |
| PATCH | `/v1/communities/{id}/channels/{channelId}` | Owner/admin; rename/reorder |
| DELETE | `/v1/communities/{id}/channels/{channelId}` | Owner/admin |

All endpoints except health and login/register require `Authorization: Bearer ACCESS_TOKEN`. Unknown communities return HTTP 404 to users who are not members. Members without manager privileges receive HTTP 403 for manager-only actions. A missing database makes the endpoints unavailable; no operation falls back to anonymous access.

## JSON examples

```json
{"name":"Gaming Hub"}
```

For channel creation:

```json
{"name":"team-voice","channel_kind":"voice","position":1}
```

Valid channel names are 2–80 lowercase ASCII letters, digits, hyphen or underscore; kinds are `text` and `voice`. The channel name is unique case-insensitively within each community. Community names must be trimmed, printable, and at most 100 characters.

For promoting/demoting a non-owner, PATCH `/v1/communities/{id}/members/{userId}` with `{"role":"admin"}` or `{"role":"member"}`. Never attempt to demote/remove the owner; owner transfer is not implemented. An admin cannot remove another admin or the owner.

Create an invitation using POST `/v1/communities/{id}/invitations` (no body). The response contains `{ "token": "...", "expires_in": 86400 }`; only a SHA-256 digest is persisted in PostgreSQL. Accept using POST `/v1/invitations/accept` with `{"token":"..."}`. Invitation redemption updates the invitation and membership in one transaction: a used, expired, already-member, or invalid invitation cannot create duplicate memberships. Do not paste invitation tokens into logs, issues, or public chat.

## Security and implementation limits

- Owner creation and membership insertion are atomic. A partial unique owner-role index prevents multiple owners; API operations do not transfer ownership or remove the owner independently.
- Sensitive mutations use membership/role predicates directly in SQL to avoid blindly trusting request IDs. Roles are always read from PostgreSQL, not client supplied JWT claims.
- No user enumeration endpoint, invitation listing, invite forwarding, private roles hierarchy beyond owner/admin/member, voice room tokens, or message CRUD is exposed.
- List endpoints currently cap output at 100 communities or 200 members/channels; pagination and search will be added later.
- Strict token scope, abuse rate limits, moderation/audit history and owner transfer are outstanding for production.

## Tests

The PostgreSQL integration suite creates three independent account sessions and checks that nonmembers cannot access channels/members, members cannot manage channels/invites, owners control role promotion, admins can create channels but not remove owner, invitations cannot be used twice, and deleting a community removes access. The existing Rust authentication, health and Windows/LiveKit tests remain in CI.