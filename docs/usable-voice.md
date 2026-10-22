# Group 4 — voice integration and recovery

[PR #82](https://github.com/justinangeloperez327/gaming-communication-platform/pull/82)
builds on the merged community/onboarding work. The Qt desktop, Rust API and
native LiveKit child remain separate. Group 4 adds recovery/consent fixes and
real synthetic audio transport assertions. Physical two-PC acceptance is still
pending; passing these checks does not complete that part of the milestone.

## Current call behavior

Select a community and voice channel. Listen only creates no microphone source.
For a microphone call, check the consent box, Join with mic, then Enable mic.
PTT uses F8; enable the mode, Enable mic, release F8, then hold it to transmit.
Mode changes mute the call. Mute/PTT stops transmission; the audio source/device
can remain open until Leave. Windows microphone privacy settings still apply.
The native connected acknowledgment waits for microphone publication/setup to
finish, so process/room startup cannot enable mic controls while audio setup is
still blocked. The existing 22-second connection watchdog covers that setup.

Consent is consumed by either join action and cleared by Leave, logout, actual
room selection changes and voice authorization/process transitions. It cannot
remain checked after a listen-only call. The shared VoicePanel contains the
actual shipped controls and is loaded by the Qt integration test.

An SDK interruption mutes and clears open-mic/PTT arming, including a reconnect
that completes between native loop polls. The native status protocol serializes
callback/session-loop output to prevent interleaved event lines. Qt cancels any
old authorization request and disables mic controls while reconnecting. After
the SDK reconnects, a fresh access check must finish before Qt marks the call
connected. Enable mic is required again. A denial, malformed response, one-second
missing mute acknowledgment, ten-second authorization timeout or fifteen-second
reconnect timeout stops the call. SDK recovery can briefly resume reception
before the HTTP check completes; this is a cooperative-client gate, not an SFU
revocation mechanism. Native mute runs at the existing 50 ms polling cadence;
SDK/driver transitions can briefly precede its application, and blocked calls
are not a hard real-time guarantee. Server reconciliation remains separate.

Terminal disconnect/device/microphone errors retain an actionable status after
the child exits. Microphone source/publication failures point to Windows privacy
settings and the selected device, without claiming which caused the failure.
Final buffered failure events are read before state reset. Authorization replies
are disconnected and aborted before controller destruction; stopped children
have stdout closed so a shutdown-time output flood cannot fill an unread buffer.
Microphone source/track owners also outlive Room during exception unwinding,
including a failing disconnect.

## What automation checks

| Layer | Real execution | Boundary |
| --- | --- | --- |
| Qt voice UI/controller | Shipped QML, HTTP and separate QProcess fixture: consent consumption, delayed audio readiness, mic/mode controls, periodic denial, immediate reconnect authorization, pending-check mute, denied recovery, missing mute acknowledgment, terminal/mic failure, unsafe URL rejection, cancellation/destruction, logout and channel deletion | The child is a protocol fixture; no SDK/network/media claim |
| Portable native policy | Fragmented/malformed commands, initial mute, consent, PTT release, open/PTT rearming after interruption, heartbeat lease and synthetic 12-hour timeline under ASan/UBSan in Linux CI | No real-time latency or long-call proof |
| Native media contract | Existing pinned C++ SDK 1.12.2 with checksum-pinned LiveKit 1.13.10 on Windows loopback: real room joins, decoded 48 kHz mono synthetic 1 kHz PCM, initial mute, remote mute/unmute, room isolation, explicit room rejoin and removal of an active publisher | Does not use PlatformAudio, real GCP accounts or physical devices; explicit rejoin is not forced SDK network recovery |
| Rust/API and stack | Database authorization/token grants, session identities, mocked occupied-room reconciliation, real SFU control API and container onboarding/community/backup/restore acceptance | The Rust reconciliation loop and physical media are not combined into one end-to-end acceptance run |
| Windows distribution | Native builds/CTest plus staged/extracted Qt/QML/SDK startup, payload/notice/hash/signature inventories and existing alpha install/repair/uninstall checks | Hosted development image, unsigned preview; clean consumer-PC acceptance remains open |

The media contract's receiver queue is capped at eight frames. It verifies a
decoded frequency component, not only a subscription callback or nonzero buffer.
After mute it allows in-flight/decoder output to drain and asserts the tone no
longer arrives; it then requires resumed tone and remote unmute signaling. The
runner checks an actual active publication before RoomService removal and waits
for SDK disconnect/departure callbacks while unrelated peers remain connected.
This is transport correctness evidence, not voice quality/latency certification.

Reproduce after a full Windows opt-in SDK build:

```powershell
ctest --test-dir build/desktop -C Release --output-on-failure
python scripts/verify_voice_media.py --executable build/desktop/Release/gcp-livekit-media-contract.exe
```

The runner downloads the official [LiveKit 1.13.10 Windows x64 asset](https://github.com/livekit/livekit/releases/tag/v1.13.10),
verifies SHA-256 `fd4b7c2c3c1e4c71adaa4f6a046ad1c0abaaaf1e09fae904b35bf231ffae11d2`,
extracts only its executable and uses disposable loopback credentials. TCP
17880/17881 and UDP 17882–17913 must be free. Processes, time/output and HTTP
responses are bounded and both children stop on failure. Admin credentials stay
in the runner/server; participant tokens travel only in the test child's
environment. No media test executable or server is added to the portable ZIP.
The SDK interfaces were checked against the [pinned SDK source](https://github.com/livekit/client-sdk-cpp/tree/v1.12.2).
Use the exact PR head's checks for results, rather than an earlier workflow run.

## Owner-run two-PC acceptance still required

Record the exact build/ZIP hash, Windows/headset/network details and observations.
Use two independently signed-in members in the same private voice channel.

| Check | Required observation | Status |
| --- | --- | --- |
| Listen only and consent | Both directions heard after explicit mic enabling; listen-only sends nothing; a stopped call cannot reuse consent | Pending |
| Windows privacy | Denied/re-enabled desktop microphone access gives useful recovery; no capture persists after Leave/logout/exit | Pending |
| Mute/PTT | Remote listener observes mute, F8 release gating, mode changes and controls while a game has focus | Pending |
| Network recovery | Real interruption before/after JWT expiry; muted recovery/access denial, manual rejoin and no silent mic enable | Pending |
| Devices | Selected headset unplug/replug, default-device changes and permission changes; no unexpected microphone switch | Pending |
| Access removal | Logout/membership/channel deletion, copied-token rejoin window and backend/SFU outage with actual occupied rooms | Pending |
| Long calls and gaming | Multi-hour calls plus repeated full-desktop/native CPU/RAM/GPU/frame-time measurement on gaming hardware | Pending |

No codec/quality defaults or application runtime dependencies changed. The
shutdown buffer and lifetime fixes remove known resource risks; no measured
CPU/GPU/RAM reduction is claimed. Text messaging is the next implementation group;
screen sharing, clean-PC qualification, attribution and owner signing remain
separate roadmap items. This work creates no release or deployed service.
