-- Ownership is assigned only by the operator's local bootstrap command.
CREATE TABLE server_ownership (
    singleton SMALLINT PRIMARY KEY CHECK (singleton = 1),
    owner_id UUID REFERENCES users (id) ON DELETE RESTRICT
);
INSERT INTO server_ownership (singleton) VALUES (1);

-- Plaintext codes are returned once; email binding does not verify email ownership.
CREATE TABLE account_invitations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email TEXT NOT NULL CHECK (email = lower(email)),
    token_hash CHAR(64) NOT NULL UNIQUE CHECK (token_hash ~ '^[0-9a-f]{64}$'),
    created_by UUID REFERENCES users (id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL DEFAULT now() + interval '24 hours',
    used_at TIMESTAMPTZ,
    revoked_at TIMESTAMPTZ,
    CHECK (expires_at > created_at)
);
CREATE INDEX account_invitations_expiry ON account_invitations (expires_at);
