# PostgreSQL foundation — Group 20

For packaged single-host operation, use the [shared server stack](server-stack.md).
`gaming-comms-server --migrate-only` applies embedded migrations and exits without
opening an HTTP listener; it requires `DATABASE_URL`. The Compose migration
service uses this command before starting an API with `GCP_RUN_MIGRATIONS=0`.

The Rust/Axum server now supports PostgreSQL using a bounded SQLx connection pool. Migrations define **users, communities, memberships, text/voice channels and messages**. The schema supports the implemented authenticated accounts, role-checked communities, messages, friends/DMs and voice-token APIs. Protected routes require database-backed bearer sessions.

## Database requirements

Use PostgreSQL 15+ and a dedicated application database/user with limited privileges. Use TLS and verified certificates when connecting over an untrusted network. Do not check database credentials into source control, logs or GitHub Actions.

Configuration (example values only, not production credentials):

```powershell
$env:DATABASE_URL = "postgres://gcp_dev:REPLACE_ME@127.0.0.1:5432/gcp_dev"
$env:GCP_DATABASE_REQUIRED = "1"
$env:GCP_DB_MAX_CONNECTIONS = "10"
$env:GCP_RUN_MIGRATIONS = "1"
cargo run -p gaming-comms-server
```

`DATABASE_URL` is optional **only for local health-only development** when `GCP_DATABASE_REQUIRED` is not set. With no database configured, `GET /health` returns HTTP 200 and `GET /ready` returns HTTP 503. Setting `GCP_DATABASE_REQUIRED=1` makes startup fail if `DATABASE_URL` is missing or cannot connect. `GCP_DB_MAX_CONNECTIONS` must be 1–32, default 10; a five-second connection-acquire timeout limits pool waits.

Set `GCP_RUN_MIGRATIONS=1` only during controlled setup or a dedicated deployment/migration job. Otherwise, **migrations do not run automatically**. For normal server processes after migration, set `GCP_RUN_MIGRATIONS=0` (or unset it). Migration files live in `apps/server/migrations` and are embedded using `sqlx::migrate!`; they are forward-only, versioned and tracked by SQLx's migrations table.

### Schema overview

- `users`: UUID key, lower-case unique handle, case-insensitive unique email index, reserved password-hash field (never plaintext), display name, timestamps.
- `communities`: UUID key, creator/owner foreign key, timestamps.
- `community_memberships`: composite key, role constraint (`owner`, `admin`, `member`), user/community foreign keys.
- `channels`: UUID key, community FK, type `text` or `voice`, position, unique case-insensitive name per community.
- `messages`: UUID key, channel/author FKs, bounded body length, creation/edit/deletion timestamps and pagination index.

**The database enforces structural integrity, not authorization.** Group 21 implements authentication/session handling, Group 22 role-based community/channel access, and Group 23 permission-checked messaging. Do not expose table CRUD without these checks.

### Endpoints

```powershell
curl http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/ready
```

`/ready` returns 204 only after PostgreSQL responds to a simple query, or 503 if the database is absent/unresponsive. No database secrets or exception details are returned to the caller.

### Tests and CI

The Rust CI job starts an **ephemeral PostgreSQL 16** service and sets `GCP_TEST_DATABASE_URL`. `apps/server/tests/postgres_schema.rs` runs the embedded schema migration, reruns it for idempotence, checks database readiness, and creates/queries linked user, membership, channel and message rows inside a rollback-only transaction. The test database credentials are for isolated GitHub CI only. Running `cargo test --workspace` without `GCP_TEST_DATABASE_URL` skips the integration test without failing other unit tests.

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace --locked
```

The Windows Qt/LiveKit build remains independent of PostgreSQL. Persistent backend sessions and text-message APIs exist; the desktop message composer is not connected. Production voice and gaming hardware certification remain incomplete.
