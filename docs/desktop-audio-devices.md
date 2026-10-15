# Group 36 — Desktop audio endpoint selection

The Windows desktop can run the existing native `gcp-livekit-voice.exe devices` probe to enumerate microphone and speaker endpoints without joining a room or opening the microphone for capture. Qt parses the bounded probe output into selection lists. Selected device IDs are passed to the native `join` command using `--input` (only with explicit microphone consent) and `--output`. Choosing Default leaves device selection to the SDK.

Scanning is disabled during an active/pending voice session. Device choices take effect on the next join. Missing IDs are rejected by the existing native validator, and a fresh scan clears selections if a device has disappeared.

**Limitations:** This does not implement live hotplug notifications, mid-call device switching, test tones, device health indication, or hardware validation. Audio quality and microphone privacy still need two-device testing. Device names and IDs are treated as untrusted probe strings, bounded before display and never used as command shells (QProcess arguments only). No automatic microphone publication.
