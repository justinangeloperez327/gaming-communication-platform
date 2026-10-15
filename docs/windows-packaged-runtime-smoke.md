# Group 49 — Runtime validation of packaged Windows binaries

The Group 48 unsigned desktop bundle now receives two extra startup checks before the CI artifact can be published.

1. **Qt/QML smoke:** Run the staged `gcp-desktop.exe --smoke-qml` with `QT_QPA_PLATFORM=offscreen`. This creates the GUI application, loads the `Gcp.Desktop` QML module from the packaged runtime and fails when the root QML object cannot be created. Unlike the legacy `--self-test` flag, it exercises the runtime Qt/QML module loader.
2. **Native voice smoke:** Run the staged `gcp-livekit-voice.exe --self-test`, which exercises the existing CLI argument checks and LiveKit SDK initialization/shutdown without obtaining a token or publishing microphone audio.

CI performs these checks after staged SHA-256 verification and before artifact upload. An unsigned bundle that cannot start its packaged Qt/QML or LiveKit modules fails the job.

**Limitations:** Hosted Windows CI does not reproduce a clean consumer Windows installation, real audio devices, a second participant, Wi-Fi changes, game frame times or code-signing trust. The QML smoke checks initial root creation only; it does not guarantee every page interaction works. Clean-machine and physical two-user acceptance remain release blockers. This PR is stacked on Group 48 PR #51.
