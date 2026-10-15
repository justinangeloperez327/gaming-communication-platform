# Group 33 — Native LiveKit connection acknowledgment

The Windows native voice process now emits fixed, non-secret protocol lines to stdout after `room.connect()` succeeds and after a normal `room.disconnect()`:

```text
GCP_VOICE_EVENT:connected
GCP_VOICE_EVENT:disconnected
```

Qt parses only complete, exact-matching lines from a bounded 8 KiB process-output buffer. All other stdout/stderr is discarded rather than shown in the UI. The desktop exposes `voiceSession.connected`, clears it when the process exits, fails to start, or Leave is requested, and avoids calling process startup a successful LiveKit connection.

**Boundaries:** SDK `room.connect()` success indicates room connection at that instant; it does not prove remote microphone audibility, continuous connectivity, physical speaker playback, or ongoing network health. Asynchronous network disconnection and automatic reconnection are not yet surfaced by the native RoomDelegate; this increment emits only connect-success and orderly-disconnect acknowledgments. No automatic rejoin is attempted. Production reconnect will require live SDK disconnect callbacks, backend membership reauthorization, renewed tokens, and explicit microphone-consent policy.

Run Windows CI and two-device physical tests before making production voice claims. No tokens, URLs, identifiers, or PCM frames belong in protocol events.
