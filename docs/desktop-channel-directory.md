# Authenticated community and voice channel directory

After sign-in the desktop fetches `GET /v1/communities`. Selection revalidates the
community with `GET /v1/communities/{id}` and loads its channels. Only voice
channels appear in the voice picker; the management editor includes both kinds.
No hardcoded room IDs or environment configuration are needed. See
[desktop communities](desktop-communities.md) for create/join/admin workflows.

Actual room selection changes stop local voice and clear pending microphone
consent. Ordinary refreshes preserve a still-valid selection; removed channels
and lost membership clear it. Listing never selects another room automatically.
Membership and channel kind are checked again at LiveKit token issuance; UI data
is not an authorization boundary.

One bounded request runs at a time, with blocked redirects, a 128 KiB response cap
and ten-second absolute event-loop deadlines. Cancel/logout/destruction abort
pending transport; generation checks discard late results. Sign-out clears roles,
members, invitations and room IDs. No directory data is persisted to disk.

The API caps communities at 100 and channels/members at 200; the selected
community can be retained as one additional verified entry. Pagination, search,
favorites and real-time list updates are absent. [Server-side eviction](server-voice-eviction.md)
is separate; connected-media eviction and cached-token rejoin timing still need
physical acceptance. Process startup is not proof of a room join or audibility.
