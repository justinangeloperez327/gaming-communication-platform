# Group 59 — Portable ZIP extraction and startup validation

The private Windows ZIP is no longer validated solely by matching the packaged file manifest. Windows CI now extracts the ZIP to a separate temporary folder, verifies its full SHA-256 file manifest, starts **the extracted** `gcp-desktop.exe --smoke-qml` in Qt offscreen mode with a 20-second timeout, and runs **the extracted** `gcp-livekit-voice.exe --self-test` and `gcp-livekit-screen.exe --self-test`. The companion `.sha256` file is checked against the actual final ZIP digest before upload.

This validates startup from the packaged ZIP layout on the hosted Windows CI runner. It does **not** prove clean consumer-PC compatibility, successful network connections, actual microphone or headset operation, two-user voice, real screen sharing, or gaming FPS/CPU/GPU/RAM targets. Those remain private testing acceptance checks; publisher signing is optional for the intended friend group.

No installer, signed release, or automatic public distribution is introduced.
