# Desktop accounts (experimental)

The account panel supports sign-in and invited account creation against the
Rust API. The [onboarding guide](account-onboarding.md) describes owner bootstrap,
invitation creation/revocation and the friend registration flow. Registration
signs in on HTTP 201; login/refresh require HTTP 200. Status messages distinguish
invitation, credential, conflict, service and transport failures.

Access and refresh tokens remain in memory. A single-shot timer refreshes about
one minute before expiry, replacing both rotated tokens. Sign-out clears local
credentials and invitation data, cancels requests, attempts bearer-authenticated
remote logout and disconnects voice. Closing the application releases local
state; it does not guarantee revocation of a still-valid server session.

Voice authorization uses the signed-in account's bearer token. The desktop lists
authorized communities and voice channels; server ownership alone does not grant
membership. Backend reconciliation periodically removes unauthorized SFU
participants; [eviction limits](server-voice-eviction.md) still apply.

Password/code fields clear after dispatch; no credentials are persisted. Only
HTTPS origins or loopback HTTP are accepted, redirects are blocked, and replies
have size caps and deadlines. Owner invitation requests are independent from
session refresh and are cancelled at sign-out. Explicit code copying uses the OS
clipboard; the application does not erase clipboard history or guarantee secure
memory zeroization.

There is no persistent login, OS credential vault, password recovery or automatic
retry after a failed refresh. A failed refresh clears the session and requires
sign-in. Qt loopback tests and API/stack tests are automated evidence; physical
desktop-to-deployed-host onboarding and media audibility are still unverified.
