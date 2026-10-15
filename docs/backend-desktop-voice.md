# Backend-to-desktop voice authorization

The desktop signs in through `/v1/auth/login`, keeps rotated access/refresh credentials in memory, loads authorized communities/channels and requests `/v1/communities/{community_id}/channels/{channel_id}/livekit-token` before each join. It does not read `GCP_ACCESS_TOKEN`, community IDs or LiveKit tokens from environment configuration.

The API checks the current bearer session, community membership, matching channel and voice kind. Its five-minute LiveKit JWT grants microphone publication and subscription, with no camera, screen or data publishing grant. The native screen-sharing diagnostics require separate operator-provided test credentials; they are not connected to this voice-only authorization route.

The desktop accepts HTTPS origins (loopback HTTP for development), refuses redirects and bounds reply buffers while receiving data. It starts the native executable with room credentials in the child environment and checks sanitized SDK room events. Do not put secrets in arguments, logs or screenshots.

See [desktop voice](desktop-voice.md) for setup and limits, [voice tokens](livekit-tokens.md) for active-SFU revocation limitations, and the [audit](stabilization-audit.md) for verification evidence.
