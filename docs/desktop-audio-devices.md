# Group 36 — Desktop audio endpoint selection

The Windows desktop can run the existing native `gcp-livekit-voice.exe devices` probe to enumerate microphone and speaker endpoints without joining a room or opening the microphone for capture. Qt parses the bounded probe output into selection lists. Selected device IDs are passed to the native `join` command using `--input` (only with explicit microphone consent) and `--output`. Choosing Default leaves device selection to the SDK.

Scanning is disabled during an active/pending voice session. Device choices take effect on the next join. Missing IDs are rejected by the existing native validator, and a fresh scan preserves a missing specific ID and reports it unavailable. Choosing Default is explicit.

Dropdowns follow the controller's selected device ID rather than a row index.
Replacing or reordering the device list preserves a selection that still exists;
failed scans preserve it too. A successful scan that no longer lists the selected
device preserves its ID and displays Saved device unavailable. A returning device
becomes available on scan without joining or restoring consent. Offline Qt/QML and subprocess tests cover selection state, model
replacement, removal and the actual input/output process arguments. They do not
establish which physical endpoint the SDK opens on a Windows PC.

Group 10 adds [per-user preference persistence and recovery](desktop-settings-recovery.md).
Audio IDs and PTT mode survive restart, with a single startup scan for specific
saved IDs and manual scans afterwards; no recurring idle scan remains. Missing
speaker selections block joining, while a missing microphone still permits
listen-only with an available speaker. No consent is restored.

**Limitations:** This does not implement live hotplug notifications, mid-call device switching, test tones, device health indication, or hardware validation. Audio quality and microphone privacy still need two-device testing. Device names and IDs are treated as untrusted probe strings, bounded before display and never used as command shells (QProcess arguments only). No automatic microphone publication.
