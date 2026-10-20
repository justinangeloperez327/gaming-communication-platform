# Long-session voice, mute and push-to-talk

The desktop voice process no longer uses the five-minute JWT join lifetime as its call duration. It joins with `--desktop-control --seconds 0` and stays in the room until Leave, logout, a connection/device failure or failed authorization/control checks. Standalone CLI diagnostics still default to 30 seconds and reject unmanaged unlimited calls. Screen diagnostic duration limits are unchanged.

## Using the desktop

1. Sign in, select a voice channel, and optionally choose devices.
2. Listen only never creates a microphone source. A microphone call still requires the per-call consent checkbox.
3. Microphone calls **start muted before publication**. After joining, select **Enable mic**.
4. With push-to-talk disabled, Enable mic permits continuous transmission until Mute mic or Leave.
5. Enable **Push-to-talk · hold F8**, then Enable mic to arm it. Release F8 first, then hold it to transmit; release it to stop transmission. The native process polls F8, so the Qt window does not need focus. Switching modes mutes the call and requires Enable mic again.
6. Mute mic overrides PTT. Leave stops the call. A reconnect resets the F8 release gate; a stopped call always requires a manual rejoin and fresh microphone consent.

The UI separates requested mute/armed state from native track mute acknowledgments. "Microphone transmitting" means the native SDK track reports unmuted, not proof that a remote user hears it. The SDK can keep the microphone device/source open while a track is muted; **mute/PTT stops transmission, not guaranteed device capture**. Use Leave to close the audio session. F8 is a fixed Windows virtual key in this increment; it may conflict with a game's binding. Configurable keys, alternate desktop/session behavior, elevated games, and physical stop latency still need validation.

## Lifetimes and authorization

- Qt sends a `ping` once per second through the child's stdin pipe. The native loop accepts a bounded set of ASCII commands: `ping`, `mute`, `unmute`, `mode_ptt`, `mode_open`, `stop`.
- A five-second heartbeat lapse, parent pipe closure, unknown/binary/oversized command or explicit stop ends the native session. This is checked by the same session thread; it is not a hard real-time guarantee if a driver/SDK call itself blocks.
- Leave queues orderly mute/disconnect and closes stdin; a two-second Qt watchdog force-stops an unresponsive process. A transmitting call that fails to acknowledge a mute within one second is also stopped.
- About once per minute Qt repeats the existing authenticated voice-token request, checking the current session and channel membership. An absolute ten-second request watchdog also covers slowly trickling responses. Denial, network failure or invalid responses stop voice; there is no automatic rejoin or silent microphone enable.
- These checks normally stop this cooperative desktop client within approximately 70 seconds of losing backend access. They are **not server-side SFU eviction** and cannot constrain a modified client or someone using a copied JWT independently.
- Recheck JWTs are discarded; this does not update the SDK's stored token. JWT join expiry remains five minutes. An established SFU session may continue beyond it, while reconnect after expiry can fail and require a new manually authorized join. Multi-hour physical calls and late reconnect have not been verified.
- Account access/refresh rotation remains handled by the existing desktop account session. No refresh token or LiveKit API secret is sent to the voice child.

The implementation keeps the existing Qt subprocess/Rust/LiveKit architecture, bounded buffers, SDK AEC/NS/AGC and codec quality settings. No additional runtime dependency, audio recording, screen capture or keyboard hook was added. F8 state polling runs at the existing 50 ms session-loop cadence; no gaming overhead improvement is claimed without measurements.

## Automated evidence and manual acceptance

`voice_control_tests.cpp` checks fragmented commands, initial mute, consent isolation, release-before-PTT, reconnect reset, manual mute precedence, mode changes, heartbeat expiry/renewal, stop, malformed commands and a synthetic 12-hour monotonic heartbeat timeline. A synthetic timeline is not a 12-hour physical call.

`voice_session_tests.cpp` uses a local HTTP fixture and a separate fake native child (no audio/LiveKit) to test real QProcess command/acknowledgment transport, account/channel setup, consent, mute, mode changes, authorization denial, listen-only isolation, Leave, pending-join cancellation and logout. The fixture executable is a test build target and is not included in the portable ZIP. Native CLI self-tests check zero-duration/managed/PTT argument restrictions. CI compiles against the pinned SDK and runs these tests plus staged/extracted ZIP startup.

Before treating long calls/PTT as accepted, test two real users on Windows: microphone/source behavior, remote audibility, held/released F8 while gaming, mode switches, manual mute, headset loss, background/elevated games, desktop suspension/exit, network loss before/after JWT expiry, account refresh and backend membership removal. Measure the complete desktop/native process overhead during matched gaming runs. No physical audio, keyboard compatibility, long-call reliability or gaming-performance result is inferred from CI.

Voice status stdout is consumed in bounded 4 KiB reads. Complete short lines are parsed even when a large diagnostic burst arrives; a status/diagnostic line longer than 8 KiB stops voice through the existing shutdown watchdog rather than silently dropping microphone-state events. Unused stderr is redirected to the null device. Offline Qt subprocess tests cover a burst of short lines, oversized output during unmute, stopped transmission state and a clean rejoin. These tests use a protocol fixture and do not verify physical microphone behavior.
