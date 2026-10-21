First unsigned Windows x64 alpha preview for private testing with friends.

Download the `setup.exe` to install for your Windows user, or the portable ZIP
to extract and run without installation. Qt and native LiveKit runtime files
are included. The GCP Rust/PostgreSQL backend and LiveKit services must be
configured separately; this download does not install or deploy them.

Available desktop features: sign-in, community/voice-channel selection,
listen-only, per-call microphone consent, mute, F8 push-to-talk, audio-device
selection and Leave. Calls start muted. Mute stops transmission; use Leave to
close the audio session. Desktop messaging and screen sharing are incomplete.

Automated CI checks cover native/Qt/Rust tests, staged/extracted ZIP startup,
installer payload integrity, installed startup, repair and uninstall on hosted
Windows. Physical two-user audio, clean consumer Windows compatibility,
gaming CPU/GPU/RAM/FPS and active-media eviction timing remain unverified.
Third-party notices are an incomplete baseline pending transitive attribution
review. This is a preview, not a production-readiness certification.

Unsigned binaries may show Windows/SmartScreen warnings. Publisher signing is
managed by the repository owner. No microphone or screen capture begins during
installation. Check release hashes against your downloaded files.
