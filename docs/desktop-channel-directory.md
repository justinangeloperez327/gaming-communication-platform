# Group 31 — Authenticated community and voice channel directory

After signing in, the desktop uses the account's bearer token to fetch `GET /v1/communities`. Selecting a community fetches `GET /v1/communities/{community_id}/channels`. Only channels with `channel_kind=voice` are offered for joining. No hardcoded community/channel IDs or environment configuration are needed.

Selection changes clear previous channel choices and disconnect any active diagnostic voice process. Backend membership and channel-kind authorization are **still repeated** at LiveKit token issuance; the UI list is not an authorization boundary.

Requests disable HTTP redirection, enforce timeouts and response size bounds, and discard late results after logout. The directory is cleared when the account signs out. No community/channel data is persisted to disk.

Limitations: no real-time list updates, no favorite channels or invitation UI, no physical two-device voice acceptance, no SFU eviction on revocation. Qt build/CI and physical audio tests are required. The native voice process start is not a verified connected-state event.
