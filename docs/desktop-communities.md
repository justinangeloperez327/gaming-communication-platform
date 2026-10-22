# Desktop communities and channels

Use a desktop and server build containing Group 3. Sign in with an existing
account; [account onboarding](account-onboarding.md) is a separate invitation
flow. Any signed-in account can create a community. Server ownership does not
grant membership or management rights in someone else's community.

## Create and join

In **Communities**, enter a name and select **Create community**. The new
community is selected and your community role is owner. Open **Community details
and management** to create a voice or text channel and generate community codes.
Creating a channel never starts capture, selects a voice channel or joins a call.

Share the community code privately with a friend who already has a GCP account.
They enter it under **Join community**. Successful redemption selects the joined
community as an ordinary member. Codes are one-use and valid for 24 hours from
creation. Unlike account invitations, community codes are not email-bound: any
signed-in nonmember holding one can redeem it.

The owner/admin can list and revoke pending codes, including those from an
earlier session. Raw codes are returned only when generated. **Copy code** writes
to the OS clipboard; signing out does not erase clipboard history. Codes clear
from the desktop on sign-out, changing community, loss of management permission,
or a refresh showing that the displayed code is no longer pending. Revocation
does not remove someone who has already joined; use member management instead.

## Roles and administration

| Community role | Desktop operations |
| --- | --- |
| Owner | Rename/delete the community; create/rename/reorder/delete channels; issue/revoke codes; promote/demote admins and remove non-owner members |
| Admin | Rename the community; manage channels and codes; remove ordinary members; leave |
| Member | View members and channels; use authorized voice channels; leave |

Open details to load members and pending invitations. These lists are fetched on
demand. Select a text or voice channel in the editor to change its name and
nonnegative position. Channel names use 2–80 lowercase letters, digits, hyphen or
underscore. Community names are trimmed and limited to 100 characters.

Removing a member, changing roles, deleting a channel, leaving or deleting the
community uses a confirmation dialog. Deleting the community also requires its
name. Confirmations retain the community identity and refuse actions after a
community switch. The owner cannot leave, be removed or demoted; owner transfer
is not implemented. Leaving requires a new code to rejoin.

The server checks current database membership/roles on each operation. Hidden
buttons and controller checks improve usability but are not authorization.
HTTP 403 triggers a read-only role refresh rather than replaying the failed
mutation. Missing membership clears selected-room and management state. Server
ownership and community ownership remain independent.

## Voice selection and failures

Choose the community and a voice channel in the sidebar before joining. The
voice picker contains only voice channels; the editor includes both kinds.
Refreshing, renaming and reordering preserve a valid selected room. Deleting the
selected channel, leaving/deleting the selected community, losing membership or
signing out clears the relevant selection and stops local voice. Switching rooms
also resets pending microphone consent. A refresh never silently selects another
community or channel. Physical audio delivery remains a separate acceptance task.

Directory requests block redirects, cap replies at 128 KiB and use ten-second
event-loop deadlines. At most one directory operation runs at a time. Cancel,
sign-out and destruction abort pending requests and discard late replies. Cancel
cannot undo a change already committed by the server: refresh before retrying a
mutation whose result is uncertain. Static errors distinguish invalid/used codes,
conflicts, permission changes, server availability and transport failures.

## Evidence and limits

PostgreSQL tests cover role boundaries, invitation scope, revocation/redemption
races, concurrent invitation capacity and member/admin self-leave. Real-container
acceptance uses two HTTP accounts for joining, promotion, channel editing,
revocation and leaving, alongside the existing persistence/restore checks. Qt
tests load the real panel and use loopback HTTP to exercise the forms, stable
selection, role changes, confirmation guards, reply bounds and cleanup. A real
QProcess fixture checks that refreshing retains a live child and deleting its
selected channel stops it. The fixture does not transmit audio.

See [PR #81](https://github.com/justinangeloperez327/gaming-communication-platform/pull/81)
for actual CI results. This does not establish physical desktop-to-deployed-host,
two-user voice, microphone privacy, screen-sharing or gaming-performance results.
No new polling, dependencies, release or deployment is added by Group 3.

The API lists the first 100 communities and 200 channels/members. The desktop
retains an explicitly selected community outside that first page after verifying
it individually, so its community list can contain 101 entries. There is no
pagination/search or live directory subscription. Details remain cached until
refresh; other clients' role/member changes are not pushed immediately. Creating
text channels is supported, but the desktop message composer remains disabled.
Owner transfer, member bans, moderation history and custom permission roles are
not implemented. Earlier published alpha downloads are unchanged.
