# Architecture

Gaming Communication Platform is a Windows-first communication application that keeps the native UI, application API, and real-time media roles separate.

## Implemented

- **Qt 6 / C++20 desktop:** Qt Quick shell with local view navigation and preview controls.
- **Rust / Axum server:** asynchronous HTTP listener and an unauthenticated `GET /health` endpoint.
- **Windows diagnostics:** Windows Graphics Capture monitor probe, WASAPI microphone probe and basic local resource measurements.
- **Optional native LiveKit diagnostic:** synthetic video and explicit Windows monitor publishing with an opt-in pinned SDK. Monitor publishing requires CPU staging readback; microphone transmission is not yet implemented.
- **Build pipelines:** CMake for the desktop, Cargo for the server. Pull-request CI compiles the Windows client and validates optional native transports.

## Intended boundaries

- **Desktop UI:** presentation, system integration, channel navigation and stream viewing.
- **Native media integration:** Windows Graphics Capture, audio devices, hardware codecs and LiveKit transport.
- **API:** identity, authenticated communities, permissions, messages, presence and media-token issuance.
- **Media delivery:** managed or self-hosted LiveKit SFU. Video/audio frames do not pass through the application chat API.
- **Persistence:** PostgreSQL for durable entities; Redis for transient state when justified; object storage for uploads.

## Design constraints

1. Do not build a bespoke WebRTC protocol.
2. Validate real capture integration, negotiated codec, and frame-copy overhead before claiming hardware-assisted streaming.
3. Keep native media processing decoupled from Qt rendering and application API.
4. Use short-lived server-issued room tokens in production; environment JWTs are only for local test binaries.
5. Keep third-party SDK downloading strictly opt-in and use pinned verified releases.
6. No fake UI capabilities: clearly label features as disconnected until working.
7. Enforce authentication and authorization before introducing private media rooms or messaging.

A modular monolith backend is the initial deployment model.

## GPU conversion prototype

The hardware-encoder diagnostic can convert a synthetic D3D11 BGRA texture to an NV12 texture on the same device using the native D3D11 video processor, then hand it to the asynchronous hardware-class encoder. The runtime hardware test is not automated in CI. Live capture and publishing remain separate paths; this work does not claim they are wired together.

## Live monitor GPU input

The opt-in hardware diagnostic now shares one D3D11 device between Windows Graphics Capture, GPU video processing and the hardware-class H.264 encoder. Captured monitor pixels remain in GPU textures; encoded output is counted and discarded locally. The independent, older LiveKit monitor-share prototype still uses CPU readback. **This new GPU capture-encoder path is not connected to LiveKit yet.**

## Opt-in encoded H.264 transport

A developer-only native bridge connects the hardware-class asynchronous H.264 MFT output callback to LiveKit `EncodedVideoSource`. It validates/normalizes encoded H.264 access units, requires explicit monitor-sharing authorization and remains independent of the Qt application. It has not been exercised on a physical Windows GPU with remote decoding. See [hardware H.264 to LiveKit](hardware-livekit.md).
