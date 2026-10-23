# Desktop friends and private conversations — Group 6

[PR #84](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84)
connects the Qt desktop to the friend/block/DM APIs and adds authenticated live DM
events. Use matching Group 6 desktop and API builds. No database migration or new
runtime dependency is required; Qt WebSockets was already packaged by Group 5.

## Friend requests

Sign in and open **Friends and private messages**. Your exact account handle is
shown as selectable text. Share it privately, then enter a friend's handle
without `@` and select **Send request**. Handles use 3–32 lowercase letters,
digits or underscores; there is no public user directory.

**Requests** shows incoming and outgoing requests with the other person's handle
and display name. Only recipients can Accept/Decline; only senders can Cancel.
The server enforces these rules even if a client bypasses the controls. Opposite
pending requests cannot be duplicated. Server-owner account invitations and
friend requests are separate: both users must already have accounts.

The friend/request/block/conversation lists refresh every 30 seconds while signed
in, or immediately with **Refresh**. They do not have push notifications. An
uncertain social mutation is never automatically repeated. **Cancel request** in
the header cancels the pending network operation, not a possibly committed
server action. Refresh to confirm the result before trying again.

## Private messages

Choose **Message** beside a friend, or open a listed conversation. The backend
opens the canonical conversation for that pair. A shared community is not needed.
The selected private conversation and a community text channel can stay active
at the same time. Neither changes the voice-room selection or starts capture.
**Close chat** clears its displayed history and stops that private subscription.

The same controller, message model and composer serve channel and private text.
History loads the latest 50 records, **Older messages** pages back, and each model
holds at most 200 records. While browsing older pages, new activity outside that
window offers Back to latest. Reconnect replaces the window with current recent
history to recover missed edits/deletions; older pages can be loaded again.

Messages use plain text and a 4000 UTF-8 byte limit. Unicode and line breaks work;
markup is displayed literally. Only an author can edit or delete their own DM.
Community ownership/admin roles grant no private-message moderation rights.
Deletion leaves a tombstone. Existing history can reappear when a removed
friendship is explicitly restored; unfriend/block do not erase stored messages.

Each send has a client UUID. **Retry send** reuses it after an uncertain response,
so a committed message cannot produce another row or overwrite a later edit or
tombstone. A frozen uncertain draft must be retried or cleared before editing.
Clear cancels the retry, not an already committed write. Up to ten private chats
retain one bounded draft each in memory; closing the application loses them.
A separate channel controller has its own ten-draft limit. There is no persistent
offline outbox or automatic application-level write retry.

## Blocking, removal and privacy

Block a friend or someone with a pending request, or Remove a friend, after the
confirmation dialog. Blocking removes the friendship and pending requests. Both
users lose DM history/write/socket access. Active subscriptions are woken after
the transaction commits, then recheck current session/friendship/block status
before forwarding activity. Every forwarded event and a ten-second periodic
check also reverify access. Unblock does not restore friendship: send and accept
a new request first. It then reopens the retained canonical conversation.

The desktop clears the selected private history and affected cached drafts when
it observes access loss; an inactive relationship may only be discovered by the
next contact refresh. Cached lists are not authorization. HTTP and socket handlers
always check the current database state. Block does **not** prevent interaction
inside shared community channels or voice rooms; the confirmation states this.

Bearer tokens use headers on the approved HTTPS/WSS origin, never socket URLs.
Only localhost/127.0.0.1 permit HTTP/WS. Redirects and TLS bypasses are not used.
Session rotation replaces socket credentials, preserves the selected peer and
revalidates metadata without replaying mutations. Logout aborts requests and
clears social state, private history, drafts and confirmations.

Messages are plaintext to the API/database, without application E2EE. Tombstones
do not erase backups/WAL or content already seen by another client. Read receipts,
unread badges, attachments, push notifications, search, typing/presence UI and DM
voice/video are absent. Event fanout is single process, without durable replay or
cross-server propagation. Complete public-internet abuse controls remain absent.

## Resource and failure limits

Friend lists hold 200 records, requests 100, blocks 200 and accessible
conversations 100, plus a still-selected conversation outside that page (101).
There is no list pagination/search. Contacts use a virtualized ListView and one
HTTP operation at a time. A refresh stages all four validated replies before
publishing the models; a malformed later reply cannot partially replace earlier
lists. These independent reads are not one database snapshot. Failed refreshes
retain cached lists with mutations disabled until a successful refresh.

Social replies are capped at 256 KiB and requests at ten seconds. Shared chat
limits remain 512 KiB for pages, 16 KiB for individual/mutation replies, 2 KiB
socket frames/messages, 256 coalesced ID notifications and one read/one mutation
per controller. Only the selected DM is subscribed. Reconnect backs off from one
to 30 seconds; HTTP polls every 30 seconds while its socket is unavailable. No
measured CPU/GPU/RAM or gaming improvement is claimed.

## Executable evidence

- `desktop_friends_and_private_scope` loads the shipped FriendsPanel and exercises
  handle/direction guards, accept/decline/cancel, peer validation, confirmed
  block/unblock, stale confirmations, staged refreshes, malformed/oversized
  replies, cancellation/destruction, logout and the actual refresh-timer/old-token
  401 race.
- Existing chat/QML loss, pagination, retry and revocation tests continue to use
  the shared composer/controller. `desktop_qml_startup` loads the complete real
  window before packaging; startup diagnostics go to captured stderr in this
  explicit test mode.
- The real Qt→Rust→PostgreSQL job uses two invited accounts for request
  cancel/decline/accept, bidirectional live DMs, author edits/deletes, offline
  resubscribe history, block/unblock/re-friend and unfriend revocation. Private
  operations preserve the independent voice selection and work after shared
  community membership is removed.
- Rust tests use three actual PostgreSQL accounts and loopback sockets for
  outsider/anonymous denial, concurrent client-ID retries, edits/tombstones,
  pair-locked edits, bilateral revocation and session logout. A channel owned by
  an unrelated user is forced to share a DM's UUID to verify event isolation.
- Windows staged/extracted runtime, portable ZIP, notice/signature inventory and
  existing alpha install/repair/uninstall checks remain in CI.

[PR checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/84/checks)
identify the tested heads and outcomes. No physical two-PC, deployed HTTPS/proxy,
audio-device, screen-sharing or gaming-performance acceptance is established.
Group 7 designs desktop screen authorization/consent; it is not implemented here.
