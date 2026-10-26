# Desktop screen viewer — Group 9

[PR #87](https://github.com/justinangeloperez327/gaming-communication-platform/pull/87)
is stacked on [Group 8 / #86](https://github.com/justinangeloperez327/gaming-communication-platform/pull/86).
The viewer uses the existing Qt Quick UI, Rust/PostgreSQL authorization and native
LiveKit screen executable. No new third-party library, media service, release,
deployment or installer implementation is added.

## Watching a friend

Sign in and choose the shared community and voice channel. In **Watch a screen**,
select **Refresh shares**, choose the publisher and select **Watch**. Discovery
uses the authenticated publisher directory. Listing or choosing a publisher does
not create a viewer lease or subscribe to media. Watch is the explicit consent
action; no consent or selection is persisted between launches.

A separate viewer window shows the publisher, community/channel, status and Stop
control. The decoded image preserves its aspect ratio, fits the window with black
bars, and handles negotiated frame sizes and 0/90/180/270-degree rotation. It does
not assume that decoded dimensions equal capture dimensions. Resize changes the
viewport without re-uploading an unchanged image. The first displayed frame,
rather than subscription success alone, changes the status to watching.

**Stop watching**, closing or hiding/minimizing the viewer window clears the
image and stops its process before network cleanup. Reopening the window does
not resume viewing: select Watch again. Logout, changed/removed community/voice
channel, lost access, lease expiry, publisher/track departure, process failure or
SDK interruption likewise stop viewing. There is no automatic switch to another
publisher or track. Switching an independent text channel or DM does not affect
the selected voice/screen scope.

The publisher and viewer use separate processes and leases. Stopping either
screen scope leaves the other scope and voice alone. Explicit Leave in the voice
panel also stops both local screen scopes. No audio subscription, microphone
capture, recording, remote control or end-to-end encryption is added by viewing.

Directory discovery is manual; it is not a live share notification feed. A listed
lease can exist before the publisher has an SFU track. Such a Watch fails with
an actionable retry status rather than displaying simulated content. The initial
supported decoder/storage profile is SDR RGBA up to 1920×1080, at most 30 presented
frames per second. The Group 8 publisher's source-size/HDR limitations remain.

## Authorization and exact-track selection

Qt validates viewer grants against the current login, channel, lease ID,
`gcp-screen-view-{lease}` identity, `gcp-screen-{channel}` room, server origin,
subscribe-only/no-publish/no-data claims and an integral 1–60-second lifetime.
Account bearers stay in Qt; grants use bounded stdin rather than argv, files,
URL queries or logs. Known inherited server/account/media credentials are removed
using the shared native child policy. Stderr is discarded; stdout is a bounded
control/frame protocol, never a persistent video log.

The new authenticated endpoint is:

`POST /v1/communities/{community}/channels/{channel}/screen-sessions/{viewer-lease}/watch`

The body contains `consent: true` and a `publisher_id` obtained from the current
directory. The server checks the exact current login, active viewer purpose,
channel membership, active publisher lease and publisher membership. It then
resolves **one** screen-video track under that exact participant in the exact
screen room using bounded SFU control I/O. Camera/audio, multiple tracks,
malformed SIDs, unknown publishers and wrong login/purpose/channel fail closed.
The response contains only the authenticated publisher identity and track SID.

The server applies `UpdateSubscriptions` for that viewer identity and SID. This
avoids enumerating the pinned C++ SDK's live, unsynchronized publication map from
a second thread. No DB locks cross SFU I/O. The server rechecks access/lease
ownership afterwards and attempts viewer removal if concurrent revocation or
rotation invalidated the request. SFU outages and cached-JWT replay still have
the limits in [screen authorization](screen-authorization.md).

The native room has `auto_subscribe = false`. It accepts only a screen-video
subscription from the directory-selected participant, pins the backend-returned
SID and opens `VideoStream::fromTrack` on that exact track. Names and remote
metadata never select a stream. A changed SID requires a new Watch action.
No automatic audio or other-publisher subscriptions are opened.

Viewer JWTs authorize subscription within the channel's screen room. They do
not cryptographically limit a modified client to one publisher. The shipped
client enforces the selected target; authorized channel members can use a
modified client or record received pixels. The application adds no recording
prevention or stronger room-level isolation claim.

## Bounded rendering and lifecycle

The decoder stream has capacity one and replaces old queued frames. One parent
credit permits one binary RGBA frame over the existing child pipe; no base64
pixels, local listener port or shared-memory name is needed. A slow UI cannot
request another frame until it consumes the current one. Width, height, rotation,
sequence and exact byte count are checked before allocation; maximum payload is
8,294,400 bytes. Headers are limited to 4 KiB and partial payloads to five seconds.
The parser yields between bounded read batches. Excess, unsolicited, malformed
or stale frames terminate the session.

Qt retains one current `QImage` and the bounded receiving payload, then uses a
SceneGraph texture for display. Image cache identity prevents reuse of an old
publisher's texture when frame sequence numbers restart. Qt owns and replaces
textures. The viewer window does not retain its SceneGraph/graphics resources
when hidden. Resize changes geometry without another upload of unchanged pixels.
Stop clears the current image and pending payload immediately. No rendering or
media polling timer runs while the viewer is idle.

This initial path includes SDK CPU-backed decode/color conversion, a bounded pipe
transfer, a copy into Qt image storage and a GPU texture upload. It is **not** a
zero-copy or proven hardware-decoding pipeline. CPU/GPU/RAM and gaming costs need
measurement; no performance budget or saving is asserted.

Grants use request-start monotonic deadlines, normal renewals around 20 seconds,
a five-second native parent heartbeat and bounded process teardown. Qt kills an
unresponsive child after 1.5 seconds; the native watchdog also handles stalled
teardown after terminal authorization/parent loss. Failed or late renewals do
not extend consent. Server cleanup is best effort; uncertain creates or failed
DELETE can leave a lease until its one-minute expiry. Closing a window, even
while an API request is pending, never rearms viewing on a late reply.

The publisher now polls feedback while waiting for fresh capture input. For a
late viewer's keyframe request on a static source, it can clone the retained NV12
image on the GPU into a fresh immutable encoder sample, with current elapsed
timing. A racing fresh capture sample is consumed first to preserve timestamp
order. This retains one bounded image, adds no CPU pixel readback, changes no
bitrate/FPS defaults and does not continuously encode an idle screen. Actual
hardware support for force-keyframe controls and late-join behavior remains an
owner-run check.

## Automated evidence and physical limits

The PR description and [current audit](stabilization-status.md) link exact-head CI
results. Test cases are distinct evidence, not an assertion that a physical
friend has seen an actual captured game:

- Portable address/undefined sanitizer tests cover viewer roles, single-use
  commands, exact SID syntax, credits, storage/rotation/sequence bounds,
  heartbeat, expiry and terminal Stop.
- Shipped QML, the actual Qt controller, QProcess pipes and SceneGraph are tested
  with an explicit protocol-only child. The tests inspect rendered synthetic
  pixels, resize/rotation, fragment/oversize/partial/unsolicited frames, bad grants,
  cancellation, expiry/revocation, visibility, logout/scope loss and forced cleanup.
  They caught and fixed a texture double deletion. Fixtures are not bundled.
- Rust/PostgreSQL and HTTP SFU fixtures check current login/purpose/consent,
  publisher/track ownership, wrong source/type/count/SID and stopped leases.
- Real Qt→Rust→PostgreSQL acceptance uses protocol-only children and the real SFU
  control service. It verifies discovery/viewer leases and refuses to fabricate
  a frame when a directory lease has no actual SFU track, preserving the separate
  publisher. It does not claim successful physical capture/rendering in that run.
- The separate Windows SDK/SFU contract also executes the **production receiver
  core** with automatic subscription disabled and a real resolved track SID. It
  verifies decoded synthetic pixels and stops that receiver when its publisher
  is removed, while unrelated synthetic voice/screen peers remain. The test
  runner supplies fixture JWTs/control calls, rather than using the Rust API or
  production Qt pipe wrapper in that same execution.
- Hosted Windows checks retain the native executable in the staged/extracted and
  installed payload and run its no-device publisher/viewer self-test. Existing
  portable ZIP, alpha install/repair/uninstall and signing-inventory checks remain.

Owner-run acceptance still needs two Windows PCs and the shared host: select a
real source, join a static share late, verify the correct publisher and current
pixels, then test window resize/minimize/close, source closure and permissions,
account/channel changes and membership removal. Stop viewing while voice and
publishing continue. Force actual SDK/network interruption and SFU/API outages,
then confirm no automatic resume, stale image/source substitution or orphaned
process. Confirm SDR colors, text/game quality, latency, decoded sizes and long
session resource growth on real GPUs/headsets and a clean consumer installation.
Gaming performance, complete attribution and owner signing remain later gates.
