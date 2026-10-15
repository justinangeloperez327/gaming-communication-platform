# Group 27 — Native Windows Voice Diagnostic

This group adds an **opt-in Windows WASAPI + LiveKit C++ SDK v1.12.2 voice prototype**, not yet a Qt desktop voice feature. Physical microphone, speaker and two-PC audio tests are outstanding. No sound samples are recorded to disk.

## Setup

Configure and build with GCP_ENABLE_LIVEKIT=ON (see LiveKit smoke-test documentation). The diagnostic executable is `build/desktop/Release/gcp-livekit-voice.exe`. Two participants must have **distinct authorized accounts** and Group 26 short-lived voice-channel tokens issued for the *same channel*.

Set the environment variables LIVEKIT_URL to the returned server_url and LIVEKIT_TOKEN to the returned participant_token (not the server API secret). Do not print or commit credentials.

## Commands (PowerShell)

```powershell
.\build\desktop\Release\gcp-livekit-voice.exe --list-devices
.\build\desktop\Release\gcp-livekit-voice.exe --join --confirm-microphone yes --seconds 60
.\build\desktop\Release\gcp-livekit-voice.exe --listen --sender REMOTE_ACCOUNT_UUID --seconds 60
.\build\desktop\Release\gcp-livekit-voice.exe --join --confirm-microphone yes --sender REMOTE_ACCOUNT_UUID --seconds 60
```

Joining **captures and publishes live microphone audio** to other participants in the room, including ambient conversations. The explicit `--confirm-microphone yes` argument is mandatory. `--listen` never opens a microphone. Use Ctrl+C to stop; sessions end automatically within 1–300 seconds. Headsets are recommended to prevent speaker echo.

Add `--input-device N` to --join or `--output-device N` to --join/--listen to use an endpoint index from the respective --list-devices list. If omitted or -1, the Windows default communications device is used. Endpoints must support shared-mode Windows format conversion to 48kHz mono PCM16 or initialization fails explicitly.

## Behavior and limits

- WASAPI event-driven 48kHz mono PCM16 capture sends 480-sample (10ms) frames through a microphone-sourced LiveKit AudioSource.
- One remote track, selected by the participant UUID from Group 26, is decoded using a LiveKit room audio callback. Callback registration occurs before connection.
- A bounded 200ms FIFO stores decoded PCM for event-driven WASAPI speaker playback. Overflow drops oldest frames, underflow generates silence. Simple mono conversion/rate adaptation is not intended as a high-quality resampler.
- No microphone files or remote audio files are created. A diagnostic prints aggregate frame/sample counts only, not tokens, private room names or contents.
- No automatic echo cancellation, noise suppression, AGC, headset hot-swap, per-person volume, push-to-talk, quality resampling, native Qt voice controls or reliable reconnection.
- The prototype cannot automatically eject an already-connected LiveKit participant when application membership is revoked; Group 26 join-token expiry is not active-session revocation.

## Physical acceptance required

On two Windows machines, run --join --confirm-microphone yes --sender OTHER_UUID with two distinct room JWTs. Verify that each person hears the other clearly, that choosing the wrong room or sender receives nothing, that listen-only never captures, that microphone permission denial fails safely, and that device disconnection/stutter/game FPS impact are measured. Do not claim production voice from hosted CI alone.

GitHub Windows CI compiles the executable and runs --help / --self-test without opening audio devices. The tests validate CLI consent checks and the bounded playback queue; **actual voice publishing and remote playback are not tested by CI**.