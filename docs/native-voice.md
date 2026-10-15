# Group 27 — Native Windows LiveKit voice diagnostic

The native C++ executable uses the pinned LiveKit SDK v1.12.2 PlatformAudio WebRTC audio device module (ADM) for microphone capture, default speaker playout, echo cancellation, noise suppression and automatic gain control. No PCM recordings, new WASAPI-to-PCM bridge or audio file output. CI cannot verify actual audio hardware or remote audibility.

## Building the optional native client

```powershell
cmake -S . -B build/desktop -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" `
  -DGCP_ENABLE_LIVEKIT=ON -DGCP_LIVEKIT_DOWNLOAD=ON
cmake --build build/desktop --config Release --parallel
ctest --test-dir build/desktop -C Release --output-on-failure
```

Run the device inventory without joining a room or publishing audio:

```powershell
.\build\desktop\Release\gcp-livekit-voice.exe devices
```

The inventory displays microphone and speaker names plus stable OS device IDs. If needed, pass the exact ID as --input or --output. Default input/output uses Windows defaults; OS microphone permissions and audio hotplug behavior require physical testing.

## Two-participant manual test

Obtain **different** short-lived LiveKit voice join tokens for two signed-in users via Group 26's:

```text
POST /v1/communities/{community_id}/channels/{channel_id}/livekit-token
Authorization: Bearer APPLICATION_ACCESS_TOKEN
```

Use the returned server_url as LIVEKIT_URL and participant_token as LIVEKIT_TOKEN; never put API secrets on the client. Both users must join the SAME voice channel. Use a local LiveKit development server or an approved remote wss:// endpoint.

**Terminal A: explicitly publish microphone**

```powershell
$env:LIVEKIT_URL = "wss://YOUR_LIVEKIT_ENDPOINT"
$env:LIVEKIT_TOKEN = "<USER_A_SHORT_LIVED_TOKEN>"
.\build\desktop\Release\gcp-livekit-voice.exe join --publish-mic `
  --confirm-microphone yes --seconds 60
```

**Terminal B: listen only (no microphone source created)**

```powershell
$env:LIVEKIT_URL = "wss://YOUR_LIVEKIT_ENDPOINT"
$env:LIVEKIT_TOKEN = "<USER_B_SHORT_LIVED_TOKEN>"
.\build\desktop\Release\gcp-livekit-voice.exe join --seconds 60
```

Optional --input DEVICE_ID (publisher) and --output DEVICE_ID (either participant). For a controlled mute test, add --mute-after 30 to a 60-second microphone-publishing run. Ctrl+C stops the session. No audio source is opened for listen-only mode.

## Physical acceptance criteria

Use headphones to avoid feedback. Verify the second person actually hears clear audio and that the microphone mutes at the expected time. Repeat using different device combinations, OS mute/privacy controls, speaker volume changes and a running game. Measure process CPU/memory, actual end-to-end voice latency, network jitter, audio glitches and game FPS impact. Repeat with two different machines, not only two windows on one machine.

The diagnostic emits a sanitized JSON record with microphone opt-in state and remote audio subscription counts. A subscription does **not** prove speaker playout or audible voice. It explicitly records remote_audio_heard_verified=false and physical_device_verified=false until manual testing. Microphone permissions, hardware reliability and remote quality remain unverified in hosted CI.

## Security and production boundaries

- Listen-only mode is default. Microphone publication requires BOTH --publish-mic and --confirm-microphone yes. A microphone source is created only after explicit consent.
- Only wss:// endpoints (or loopback ws://localhost or ws://127.0.0.1 for developer tests) are accepted. Tokens must be short-lived, backend-issued, microphone-scoped. Never log real room JWTs.
- SDK-provided PlatformAudio uses WebRTC ADM and may continue capturing while the local track is muted. To prevent further physical capture, end the session or disable microphone OS access; mute is not equivalent to hardware permission revocation.
- The backend still does not eject already connected participants from the SFU when community membership is revoked. Group 27 does not add that security control.
- No push-to-talk, participant mixer, per-user volume, microphone voice activity indicator or E2EE. A Qt control surface, bounded SDK reconnect handling and selected-device loss detection exist; default-device rerouting and physical recovery behavior remain unverified.
- Native Windows CI compiles the executable, runs CLI consent/SDK self-tests and uploads the DLLs. CI neither joins a real LiveKit room nor verifies physical microphone/speaker operation. No production voice claim should be made from CI alone.
