# Group 34 — Fail-closed voice recovery guardrails

The Qt client now enforces a **22-second room-acknowledgment deadline** after the native voice process starts. If the native process never emits the exact `GCP_VOICE_EVENT:connected` acknowledgment, Qt terminates it, forcibly kills it after two seconds if necessary, and shows a distinct timeout/error status. This complements the existing 12-second executable startup timeout.

After a timeout, the user must deliberately choose **Listen only** or explicitly reauthorize microphone publication using the existing consent control. Every attempt requests a fresh backend-authorized LiveKit room JWT. There is no automatic retry, background microphone reopening, or token reuse.

This is an initial recovery safety increment rather than continuous network resilience. Current native events only acknowledge initial `room.connect()` and orderly `room.disconnect()`. The SDK may lose the connection asynchronously without emitting a protocol event; these scenarios **cannot yet be reliably detected by Qt**. Before implementing automatic reconnect, verify the pinned SDK's exact RoomDelegate connection lifecycle APIs, forward native disconnect/reconnect events, recheck server-side membership for every reconnection, and ensure revocation actually ejects participants from the SFU.

CI is hardware-free: test on two Windows machines with microphone permissions, network interruption, device hotplug and game-frame-time instrumentation. Never treat room acknowledgment as proof of audible remote audio.
