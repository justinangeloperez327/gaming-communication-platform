# Authentication API — Group 21

The Rust/Axum server supports account registration, login, rotating refresh tokens, logout and the current user's profile. Sessions are stored in PostgreSQL. This is an integration-tested security foundation, **not suitable for public exposure without TLS, distributed rate limiting, monitoring and further hardening**.

## Security properties

- Argon2id password hashing: random salts, 64 MiB memory, 3 passes, single lane; bounded blocking workers instead of Tokio reactor blocking.
- Independent 32-byte CSPRNG access and refresh tokens, returned only at issuance/rotation. PostgreSQL stores SHA-256 digests, not plaintext tokens.
- Access expires after 15 minutes. Refresh tokens expire after 30 days, with an absolute refresh lifetime; refresh rotates both tokens atomically and invalidates the former access token.
- Bearer access token required to read/update own profile or revoke a session. No credentials in URLs or application logs.
- Axum limits incoming bodies to 8 KiB. Generic error responses do not expose passwords, tokens or SQL errors.
- Missing database makes authentication endpoints unavailable rather than unauthenticated.

## Endpoints

| Method | Route | Requirements |
| --- | --- | --- |
| POST | /v1/auth/register | handle, display_name, email, password |
| POST | /v1/auth/login | email, password |
| POST | /v1/auth/refresh | refresh_token |
| POST | /v1/auth/logout | Authorization: Bearer ACCESS_TOKEN |
| GET | /v1/me | Authorization: Bearer ACCESS_TOKEN |
| PATCH | /v1/me | Bearer token plus display_name |

Register handle: 3–32 lowercase alphanumeric/underscore; trimmed display name 1–80 characters; basic ASCII email with case-insensitive uniqueness; password 12–128 bytes. Email verification is **not** implemented.

Register/login/refresh return JSON with user, access_token, refresh_token, token_type, expires_in. Logout returns HTTP 204. Keep tokens private and use HTTPS for anything outside loopback.

## Local setup

Run the versioned sessions migration with GCP_RUN_MIGRATIONS=1 using the PostgreSQL setup described in [postgresql.md](postgresql.md), then unset that flag on ordinary server instances.

Example loopback-only local request:

```powershell
$env:DATABASE_URL = 'postgres://gcp_dev:REPLACE_ME@127.0.0.1:5432/gcp_dev'
$env:GCP_DATABASE_REQUIRED = '1'
$env:GCP_RUN_MIGRATIONS = '1'
cargo run -p gaming-comms-server
```

To register, POST JSON to http://127.0.0.1:8080/v1/auth/register with fields handle, display_name, email, password. Send the returned access token in the Authorization Bearer header for GET or PATCH /v1/me, and the refresh token in the POST /v1/auth/refresh JSON body.

Do not print or commit real credentials. Windows desktop applications should use Windows Credential Manager/DPAPI for token storage, not plaintext configuration.

## CI

CI provisions PostgreSQL 16, runs migrations and tests registering, case-insensitive duplicates, login failures and success, read/update own profile, token rotation, invalidated old access/refresh, logout and session digest storage.

## Not yet production complete

Distributed login/IP throttling, user enumeration timing controls, email ownership verification, password reset, MFA, device-level session management, audit trails, account deletion and proactive compromised-password screening are not provided yet. They must be addressed before internet exposure. Community permissions, authenticated messages and authorized LiveKit voice tokens are implemented in their documented API routes.