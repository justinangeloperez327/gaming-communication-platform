# Architecture

Gaming Communication Platform is a Windows-first communication application that keeps the native UI, application API, and real-time media roles separate.

## Implemented

- **Qt 6 / C++20 desktop:** Qt Quick shell with local view navigation and preview controls. The media engine and network session are not connected.
- **Rust / Axum server:** asynchronous HTTP listener and an unauthenticated `GET /health` endpoint.
- **Separate build pipelines:** CMake for the desktop, Cargo for the server. Pull-request CI builds the Windows client and tests the server.

## Intended boundaries

- **Desktop UI:** presentation, system integration, channel navigation, stream-view presentation.
- **Native media integration:** Windows Graphics Capture, audio devices, hardware codecs, and WebRTC SDK.
- **API:** identity, authenticated communities, permissions, messages, presence and media-token issuance.
- **Media delivery:** managed or self-hosted LiveKit SFU. Video/audio frames do not belong in the application chat API.
- **Persistence:** PostgreSQL for durable entities; Redis for transient state where justified; object storage for uploads.

## Design constraints

1. Do not build a bespoke WebRTC protocol.
2. Validate media SDK compatibility and frame-copy overhead in a narrow prototype before committing to a custom encoding pipeline.
3. Keep the native media path decoupled from QML rendering and the backend API.
4. Version network contracts before adding separate client/server deployments.
5. Avoid creating fake functionality: interfaces should accurately state whether a feature is connected.
6. Enforce authentication and authorization before introducing private media rooms or persistent messaging.

A modular monolith backend is the initial deployment model. This is not a microservices project.
