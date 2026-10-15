# Group 48 — CI-built unsigned Windows desktop bundle

Groups 46 and 47 (PRs #49 and #50) are merged; their CI, benchmark tooling, and package-integrity tests passed.

The Windows build job now stages the Qt desktop and LiveKit voice executable, uses `windeployqt` for Qt/QML runtime dependencies, verifies every staged file against the bundle's SHA-256 manifest, and only then uploads a seven-day **unsigned diagnostic artifact** named `gcp-desktop-unsigned-test-win64`. The manifest writer explicitly emits an array for the verifier.

## Verification boundary

CI validates packaging mechanics on the hosted Windows builder. It does not establish clean-machine runtime compatibility, Microsoft code-signing trust, microphone permission behavior, network/LiveKit connectivity, physical voice audio, gaming FPS, or product release suitability.

If the packaging stage fails, inspect the Windows CI log for missing Qt QML modules, `windeployqt` issues, and the exact native runtime DLL requirements. Do not disable verification to force artifact publishing.

Before distributing to testers, execute the published artifact on a separate clean Windows PC, verify `SHA256-MANIFEST.json`, test login and voice with two physical headsets/participants, and confirm redistributable licensing requirements. No version tag, installer, deployment, or release is created.
