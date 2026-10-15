# Group 28 — Desktop voice control bridge (experimental)

The Qt desktop shell can start/stop the optional native LiveKit voice diagnostic from the desktop UI.

## Setup (Windows developer environment)

Build with `-DGCP_ENABLE_LIVEKIT=ON` (see [native voice](native-voice.md)). Place `gcp-livekit-voice.exe` and its dependent LiveKit runtime DLLs beside `gcp-desktop.exe`. Obtain a short-lived token from the authenticated backend route `POST /v1/communities/{community_id}/channels/{channel_id}/livekit-token` and start the desktop process with `LIVEKIT_URL` and `LIVEKIT_TOKEN` in its process environment. **Never add these credentials to repository files, screenshots or command-line arguments.**

The desktop UI supports listen-only, explicit per-session microphone consent, and Leave. The microphone checkbox automatically resets after a join request. All voice calls are currently limited to **300 seconds** by the native diagnostic. Leaving terminates the process, followed by a forced stop if needed. The UI never renders child-process stdout, stderr or token contents.

## Not production ready

This is a subprocess bridge, **not** integrated application login or backend token retrieval. Environment tokens can expire; they are not refreshed. The UI does not confirm actual LiveKit connection, remote audibility or physical device usage, and a started process is not proof of a joined room. Voice/device handling remains in the pinned native SDK. A production client needs secure in-memory login/session handling, just-in-time room credentials, successful-join state feedback, reconnect, expiring-token recovery, revocation-aware disconnect, device selection, push-to-talk, real hardware acceptance tests and runtime DLL packaging. User-approved microphone capture can continue until Leave or diagnostic timeout.
