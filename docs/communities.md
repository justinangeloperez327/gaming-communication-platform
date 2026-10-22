# Communities, channels and permissions

Only authenticated PostgreSQL-backed sessions may access communities. Every community has one immutable owner; an owned community must be deleted before that account can be deleted. Owner, admin and member roles govern management capabilities. Authorized text message endpoints are documented in [messages](messages.md).

The [desktop management panel](desktop-communities.md) connects these operations
to account-based create/join, channel editing, invitations and member management.

## API routes

| Method | Route | Permissions |
| --- | --- | --- |
| POST | `/v1/communities` | Any signed-in user; creates community plus owner membership in one transaction |
| GET | `/v1/communities` | Signed-in user; lists first 100 of their own communities |
| GET | `/v1/communities/{id}` | Community member |
| PATCH | `/v1/communities/{id}` | Owner or admin; rename |
| DELETE | `/v1/communities/{id}` | Owner only, cascades channels and memberships |
| DELETE | `/v1/communities/{id}/membership` | Signed-in member/admin leaves; owner cannot leave |
| GET | `/v1/communities/{id}/members` | Member; limited to 200 rows |
| PATCH | `/v1/communities/{id}/members/{userId}` | Owner only; assign admin/member, never owner |
| DELETE | `/v1/communities/{id}/members/{userId}` | Owner can remove non-owner; admin can remove members only |
| POST | `/v1/communities/{id}/invitations` | Owner/admin creates a one-use 24-hour invite |
| GET | `/v1/communities/{id}/invitations` | Owner/admin; first 200 pending identifiers/lifetimes, never raw codes |
| DELETE | `/v1/communities/{id}/invitations/{invitationId}` | Owner/admin; deletes an unused code within this community |
| POST | `/v1/invitations/accept` | Any signed-in, nonmember user with valid unused invite token |
| GET | `/v1/communities/{id}/channels` | Member; limited to 200 rows |
| POST | `/v1/communities/{id}/channels` | Owner/admin; create text or voice channel |
| PATCH | `/v1/communities/{id}/channels/{channelId}` | Owner/admin; rename/reorder |
| DELETE | `/v1/communities/{id}/channels/{channelId}` | Owner/admin |

All community endpoints require `Authorization: Bearer ACCESS_TOKEN`. Unknown communities return HTTP 404 to users who are not members. Members without manager privileges receive HTTP 403 for manager-only actions. A missing database makes the endpoints unavailable; no operation falls back to anonymous access. Server ownership does not bypass community roles.

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

Create an invitation using POST `/v1/communities/{id}/invitations` (no body). The response contains `{ "id": "...", "token": "...", "expires_in": 86400 }`; only a SHA-256 digest is persisted in PostgreSQL. Accept using POST `/v1/invitations/accept` with `{"token":"..."}`. Invitation redemption updates the invitation and membership in one transaction: a used, expired, already-member, or invalid invitation cannot create duplicate memberships. Do not paste invitation tokens into logs, issues, or public chat.

Pending lists expose only IDs and remaining lifetimes. Revocation deletes an
unused code; redemption racing revocation can either create membership or be
rejected, never both succeed. Revocation does not remove existing membership.
Redemption also requires the creator to be a current community owner/admin.
Community codes are not email-bound; they require an existing signed-in account.
Account registration invitations are [separate](account-onboarding.md).

Issuance serializes a 200-active-code limit per community and prunes records
expired more than seven days ago. Older databases can already contain more than
200 pending codes; no active codes are silently discarded. Revoke entries on the
current page and refresh to reach later entries. Issuance resumes below the cap.
These changes reuse the existing invitation schema; no new migration is needed.

## Security and implementation limits

- Owner creation and membership insertion are atomic. A partial unique owner-role index prevents multiple owners; API operations do not transfer ownership or remove the owner independently.
- Sensitive mutations use membership/role predicates directly in SQL to avoid blindly trusting request IDs. Roles are always read from PostgreSQL, not client supplied JWT claims.
- Public user enumeration, custom permission roles, member bans and owner transfer are absent. Voice grants and message CRUD are covered by their separate authorized endpoints.
- List endpoints currently cap output at 100 communities or 200 members/channels; pagination and search will be added later.
- Strict token scope, abuse rate limits, moderation/audit history and owner transfer are outstanding for production.

## Tests

PostgreSQL tests cover nonmember/member/admin/owner boundaries, protected owner
membership, channel management, one-use invitations, community-scoped revocation,
revocation/redemption races, concurrent invitation limits and member/admin leave.
Real-container acceptance also exercises joining, promotion, channel changes,
revocation and leaving. Actual QML/HTTP and voice-child fixtures cover the desktop;
none establishes physical audio delivery or production deployment acceptance.
