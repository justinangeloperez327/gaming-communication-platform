# Private account onboarding

New accounts require an invitation from the server owner. Ownership is configured
by an operator on the shared host, never by the first HTTP registration or a
client-supplied role. Existing accounts and sessions survive this migration;
existing users can still sign in. Community membership invitations are separate
and do not create accounts or grant server ownership.

## Configure the owner

Bring up the [server stack](server-stack.md), including migrations, then run:

```bash
python3 scripts/server_stack.py --config /srv/gcp/config owner \
  --email owner@example.com --handle owner --display-name "Server owner"
```

Use the configuration directory selected at initialization. The tool prompts for
the password twice without echo; it refuses an input environment where hiding
the password is unavailable. Passwords must contain 12–128 UTF-8 bytes. They are
sent to the container over standard input, not command-line arguments or an
environment variable. Do not paste real passwords into shell commands or logs.

For an existing account, supply its current password and exact handle. Bootstrap
preserves that account's profile and sessions. Incorrect credentials or an
already configured owner cause refusal. Concurrent bootstrap attempts serialize
on the ownership row; only one can succeed. There is no HTTP bootstrap endpoint,
owner transfer UI or automatic promotion of an existing first user.

Native operators can run `gaming-comms-server --bootstrap-owner` after migrations
with `DATABASE_URL` configured. It reads one JSON object containing `email`,
`handle`, `display_name` and `password` from standard input, capped at 8 KiB, and
exits without starting an HTTP listener. Prefer the stack's hidden prompt rather
than composing a command containing a real password.

## Invite friends from the desktop

1. Sign in to the updated desktop using the owner's email and password.
2. Under **Invite a friend**, enter the friend's email and select **Generate
   code**. Share the server address and code privately. GCP sends no email.
3. The friend selects **Create account**, enters that same email, a handle,
   display name, password and confirmation, and the invitation code. A successful
   registration signs them in immediately.
4. The owner can refresh and revoke pending invitations, including those created
   by an earlier desktop session. Revoking an invitation does not delete an
   account that has already registered.

Codes contain 256 random bits, expire after 24 hours, and are bound to the
canonical email. They can be used once. Only the issuance response returns the
raw code; pending lists contain identifiers, email and remaining lifetime. The
desktop keeps the latest generated code in memory until sign-out or replacement.
**Copy code** explicitly writes it to the OS clipboard; signing out does not
erase clipboard history or copies shared with someone else. Lost codes can be
revoked and replaced.

The server permits at most 128 active invitations. Creating a new invitation
also prunes records expired for more than seven days. Invalid, missing,
wrong-email, expired, revoked and used invitations receive HTTP 403 before
password hashing. Consumption, account creation and initial session creation
commit together. A duplicate account rolls back consumption; concurrent reuse
can create at most one account. Invitations do not bypass normal input validation
or community/channel authorization.

## Desktop behavior and API

The form checks confirmation and input bounds, clears password/code fields after
dispatch, and offers cancellation. Tokens remain in memory; login persistence
and OS credential storage are not implemented. Registration accepts HTTP 201;
login and refresh accept HTTP 200. Static errors distinguish rejected invitations,
duplicate accounts, invalid credentials, unavailable services and network/TLS
failures without displaying arbitrary server error text.

Authentication replies are capped at 16 KiB; invitation management replies at
64 KiB, with ten-second absolute event-loop deadlines and blocked redirects.
Invitation requests use a separate bounded reply from session refresh. Sign-out
cancels pending work and clears owner/code/list state. Destruction aborts requests
before session state is destroyed. These controls do not guarantee zeroization
of Qt, allocator or OS memory.

| Method | Route | Access / input |
| --- | --- | --- |
| POST | `/v1/auth/register` | Existing account fields plus `invitation_token` |
| POST | `/v1/account-invitations` | Owner bearer token; JSON `email` |
| GET | `/v1/account-invitations` | Owner bearer token; active invitations only |
| DELETE | `/v1/account-invitations/{id}` | Owner bearer token; invitation UUID |

Session responses include `server_owner` for desktop presentation. The server
independently checks database ownership on every invitation operation; a forged
client flag grants no permissions. Server ownership does not grant implicit
community membership or change media permissions.

## Evidence and remaining limits

PostgreSQL tests cover bootstrap races, existing-account ownership, email binding,
hashed code storage, atomic consumption, conflict rollback, concurrent reuse,
expiry, revocation and owner-only access. Real-stack acceptance exercises the
operator binary, owner login, invited registration and owner preservation after
backup/restore. Qt tests exercise the actual QML registration form and loopback
HTTP invitation controls, error handling, cancellation and destruction. See
[PR #80](https://github.com/justinangeloperez327/gaming-communication-platform/pull/80)
for the build and test results. These are not physical desktop-to-deployed-host,
two-user voice, screen-sharing or gaming-performance acceptance.

Use desktop and server builds containing this change. The earlier published alpha
does not gain these controls automatically; this work does not publish a release.
Email delivery/verification, password reset, MFA, owner transfer, account deletion
and persistent login remain absent. Keep the private source-IP boundary and
HTTPS/WSS; invitations do not supply complete public-internet abuse protection.

Backups include ownership, invitation state and sessions. Restoring an earlier
snapshot can reactivate a code or session revoked/used after that snapshot.
Review and revoke restored pending codes and sessions before reopening access.
