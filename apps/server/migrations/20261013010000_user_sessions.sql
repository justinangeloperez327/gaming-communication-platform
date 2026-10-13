-- Persistent, revocable bearer sessions. Store only SHA-256 token digests.
-- Access expires after 15 minutes; refresh expires 30 days after issuance.
-- A refresh rotates both tokens atomically and preserves absolute lifetime.
CREATE TABLE user_sessions (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    access_hash CHAR(64) NOT NULL UNIQUE,
    refresh_hash CHAR(64) NOT NULL UNIQUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    access_expires_at TIMESTAMPTZ NOT NULL,
    refresh_expires_at TIMESTAMPTZ NOT NULL,
    rotated_at TIMESTAMPTZ,
    revoked_at TIMESTAMPTZ,
    CHECK (access_expires_at <= refresh_expires_at),
    CHECK (refresh_expires_at > created_at)
);
CREATE INDEX user_sessions_user_id_idx ON user_sessions (user_id, created_at DESC);
CREATE INDEX user_sessions_refresh_expiry_idx ON user_sessions (refresh_expires_at)
    WHERE revoked_at IS NULL;
