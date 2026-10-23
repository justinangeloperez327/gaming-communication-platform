# Group 35 — SDK network lifecycle events

The native LiveKit RoomDelegate forwards non-sensitive `reconnecting`, `reconnected`, and `connection_lost` events over the existing exact-match stdout protocol. Qt tracks interrupted room connectivity separately from native process state. A reconnecting session receives a 15-second deadline; if it cannot recover, Qt terminates the native subprocess. Definitive SDK disconnect also tears down native voice.

The SDK itself handles in-session reconnection; the desktop does **not** spawn a new process or obtain/reuse a fresh token automatically. Once the session is terminated, the user must manually join again, requesting a new backend-authorized token and (if publishing) fresh microphone consent.

The later [Group 4 work](usable-voice.md) mutes and clears arming on interruption,
rechecks current access immediately after SDK recovery and requires Enable mic
again. Qt/QProcess fixtures cover those transitions; a separate synthetic PCM
contract checks actual room joins, explicit rejoin and active-peer eviction.

**Physical acceptance still pending:** pinned SDK callbacks compiled on Windows CI; actual network interruption, SDK reconnect behavior, microphone mute during a transient reconnect, active session revocation and two-device audio. This is an incremental network lifecycle integration, not production certification.
