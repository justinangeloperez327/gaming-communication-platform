# Group 30 — Desktop account sign-in (experimental)

The Qt desktop now accepts a GCP server origin, email and password, and exchanges them against the Rust backend `POST /v1/auth/login`. Account access and refresh tokens are held only in process memory. A single-shot timer refreshes the session through `POST /v1/auth/refresh` approximately one minute before access expiry, replacing both rotated tokens. Sign-out clears local credentials immediately, cancels pending requests, attempts server revocation through the bearer-authenticated `POST /v1/auth/logout`, and disconnects voice.

Voice token requests now use the signed-in account's in-memory bearer token, instead of `GCP_ACCESS_TOKEN` or `GCP_API_URL` environment values. The development-only `GCP_COMMUNITY_ID` and `GCP_VOICE_CHANNEL_ID` remain required for room selection; these are not account secrets.

## Limits

No persistent login or OS credential vault. Closing the client signs out; server logout is best effort if offline. The app does not yet list communities/channels, recover interrupted refreshes, or support account registration. Username/password fields are not persisted; do not collect credentials through logs. Only HTTPS origins and loopback HTTP are accepted; redirects are not followed. Authenticated account status is not a verified media connection indicator. Active SFU participant revocation is still outstanding.

CI build and runtime validation required before merge. Do not deploy as production login.
