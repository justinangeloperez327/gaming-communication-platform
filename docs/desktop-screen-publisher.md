# Desktop screen publisher — Group 8

[PR #86](https://github.com/justinangeloperez327/gaming-communication-platform/pull/86)
is stacked on [Group 7 / #85](https://github.com/justinangeloperez327/gaming-communication-platform/pull/85).
It adds the publisher to the existing Qt/native architecture. The desktop viewer
is Group 9; another GCP desktop cannot yet display this broadcast. No release,
deployment, version change or new installer implementation is part of this work.

## Using the publisher

Build the Windows desktop with `GCP_ENABLE_LIVEKIT=ON` and the pinned SDK. Use the
shared staging/ZIP scripts so `gcp-livekit-screen.exe`, `gcp-livekit-voice.exe`,
`livekit.dll`, `livekit_ffi.dll`, Qt/QML and the app-local CRT travel together.
The existing server stack must have Group 7's screen-lease migration applied
and working LiveKit control readiness. No server credentials belong on friends'
computers.

Sign in, select a community and voice channel, then select **Choose source**.
The native child opens Windows GraphicsCapturePicker for one window or display.
Picking retains that exact `GraphicsCaptureItem`; it does not start capture or
request a token. Qt shows its title/dimensions and the selected community/channel.
Check the authorization box and select **Start sharing** to request a publish
lease. Cancel closes the child without requesting a lease.

The Stop control remains available throughout selection, authorization, startup
and publication. **Stop sharing** closes this screen process and invalidates its
lease while leaving voice alone. Explicit **Leave** in the voice panel also stops
your screen. Logout and a changed or removed community/voice channel stop it.
Changing the independent text channel or private conversation does not stop it.

The status moves through authorization, startup, capturing, then sharing after
the first IDR is accepted by the SDK. This proves local SDK submission only;
it does not prove delivery, remote rendering or that a friend is watching.
Windows' normal capture border is retained. There is no background permission
bypass, automatic new source selection, disk recording or system audio capture.
Members with a compatible viewer can record the content they receive.

## Supported profile and failure behavior

The initial profile is SDR, 30 fps maximum, 4 Mbps initial target, and source sizes
160×90 through 1920×1080. Odd source dimensions are converted on the GPU to the
nearest even NV12 output size, using the entire source rectangle. This is a tiny
rescale, not a crop. Larger, smaller or resized sources require a new selection;
1440p/4K downscaling, HDR/tone mapping and product quality controls are not added.
BGRA8 on an HDR display can have incorrect color; use an SDR source/display.
Protected/minimized windows, OS capture restrictions, unsupported hardware,
multiple-adapter behavior and device removal require physical qualification.

The retained source feeds Windows Graphics Capture → D3D11 video processor →
GPU NV12 samples → asynchronous hardware-class Media Foundation H.264 → LiveKit
`EncodedVideoSource`. There is no raw-pixel CPU readback or software-encoder
fallback. WGC uses two buffers and one replaceable latest NV12 sample; old pending
frames are discarded. Only encoded bytes cross into the SDK. Conversion/submission
runs at most at the chosen frame rate. Timestamps preserve elapsed static-screen
gaps. The encoder continues servicing output while a fresh input frame is
unavailable, with bounded input credits/backlog and event queues. Asynchronous samples own distinct surfaces
until the encoder releases them. Existing finite diagnostics remain available.

Closing/resizing the source, capture/encoder failure, SDK reconnect/disconnect,
invalid/expired authorization or lost parent heartbeat ends the session. Choose
again and confirm again to retry; consent is never rearmed automatically. Source
resize deliberately stops instead of silently changing the content or resolution.
No capture runs while idle. Static sources may deliver fewer than 30 frames/s.

## Credentials, deadlines and cleanup

Qt keeps the account bearer token. It sends the short-lived LiveKit grant to the
child only through bounded stdin, never argv, a file, a URL query or a diagnostic
log. Known inherited database, LiveKit and GCP media/account credentials are
removed, including mixed-case names. Stderr is discarded; stdout accepts only
bounded fixed protocol events and base64 source metadata, rendered as plain text.

The client checks the grant's purpose, lease ID, identity, exact screen room,
source-only/no-subscription/no-data JWT claims, endpoint and integral 1–60-second
lifetime. SFU signature verification remains the SDK/server's responsibility.
Requests use bearer headers, manual redirects, bounded replies and deadlines on
the approved HTTPS origin (literal loopback HTTP is allowed for development).
There is one create/renew request at a time and no automatic create replay after
an uncertain response. Token refresh can defer renewal within the existing lease
deadline; denial still stops capture and requires explicit retry.

The maximum local lease deadline is request-start monotonic time plus the
returned lifetime, so HTTP/IPC delay cannot extend it. Renewals are normally every
20 seconds or earlier for shorter leases. The child receives the same absolute
steady-clock deadline; it independently enforces expiry and a five-second parent
heartbeat limit. Invalid protocol input is terminal. Qt bounds stop with a 1.5s
kill watchdog; the child also terminates itself if teardown stalls over two seconds
after a terminal condition. These guards bound this process, not frames already
queued elsewhere or SFU enforcement during an outage.

Local consent is cleared before network cleanup. The UI reports stopping until
the child exits; a child claiming `stopped` cannot keep running indefinitely.
DELETE is best effort with a three-second deadline. A failed or cancelled create
may leave an unknown server lease, and failed cleanup may leave the known lease;
both expire within a minute. An uncertain create is never retried automatically.
DELETE acceptance does not prove that the SFU has already disconnected a peer.
See [screen authorization](screen-authorization.md) for periodic revocation,
control outages, cached-JWT replay and exact-login ownership limits.

## Evidence and pending acceptance

- Portable consent/control tests run with address/undefined sanitizers in CI:
  source prerequisite, one-use grant, Stop, invalid scope/protocol, heartbeat loss,
  renewals and expiry. No devices or network are opened by these tests.
- The shipped ScreenPanel and ScreenSession run with actual HTTP, QProcess pipes,
  timers and a clearly identified test-only native peer. Cases include no-consent
  and cancel, scope/source loss, bad grants, oversized replies/output, stale
  replies after Stop, renewal denial/expiry, startup failure and forced cleanup
  of a hung child, plus actual timed account-token rotation. The fixture is excluded from distribution.
- Linux acceptance runs the real Qt controller against Rust/PostgreSQL and the
  checksum-pinned real SFU's control service. It checks real signed lease
  create/renew/stop, ownership directory and membership-loss shutdown. Its native
  peer simulates protocol events; it captures/publishes no pixels.
- The separate existing Windows SDK/SFU contract decodes synthetic video and PCM,
  enforces source/room separation and removes only the selected screen publisher.
  It does not exercise this OS picker, GPU hardware path or Qt viewer.
- Windows CI compiles the actual publisher, runs no-device protocol/H.264 self
  tests, requires its executable in the staged/extracted manifest, tests missing
  dependency rejection, and runs self-tests in staged/extracted/installed payloads.

Exact checked-head results belong in the [current audit](stabilization-status.md)
and PR checks. Passing automated fixtures is not physical screen acceptance.

Owner-run before qualifying the product: two Windows computers with an authorized
receiver; choose each window/display and cancel; confirm correct content and
capture border; deny Windows permission; close, minimize and resize the selected
source; switch accounts/channels and remove membership; stop while the API/SFU is
unreachable; close/kill Qt during active capture; unplug/change GPUs/displays and
force real SDK/network interruptions. Confirm no substituted source, no automatic
restart, bounded process cleanup, correct remote dimensions/color, and continuing
voice after screen-only Stop. Group 9 must supply the product viewer first.
Measure matched gaming CPU/GPU/RAM/frame times and long-session resource growth on
actual hardware, then qualify clean consumer Windows, signing and attribution.
