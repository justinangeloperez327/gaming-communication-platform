# Group 32 — Voice process lifecycle reliability

The desktop voice subprocess has a 12-second startup watchdog, a 2-second graceful shutdown deadline followed by a forceful kill, and explicit handling of normal process termination, crashes, and failed launches. Pending backend authorization is canceled on Leave. Shutdown timers are managed as members of the Qt session object rather than detached single-shot closures, so a previous stop attempt cannot accidentally kill a later session.

The desktop clears its stored child-process environment after launch confirmation or failure, reducing lifetime of the LiveKit token copy in Qt's process configuration. This does **not** scrub environment values already inherited by the native process or other transient Qt memory copies.

The UI shows **native voice process started**, not **connected**: actual room participation and audible playback are not verified by process state. Automatic reconnection is intentionally disabled until native SDK callbacks can report reliable connection state and a backend membership recheck can authorize every retry. The existing microphone consent gate still applies for each attempt.

## Physical and security acceptance still pending

- Simulate slow startup, failed native executable, unexpected crash and Leave during authorization.
- Confirm microphone hardware stops on Leave or timeout; test Windows device removal and return.
- Test two physical devices and measure actual end-to-end audio and game FPS effects.
- Add LiveKit SDK callback-driven connected/reconnecting/disconnected events and backend-authorized reconnect.
- Add active SFU participant eviction when membership/session authorization is revoked.

This is incremental lifecycle hardening, not a claim of production-ready voice.
