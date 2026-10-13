-- Single-use, short-lived community invitations. Only a hash of each
-- randomly generated token is retained; token plaintext is response-only.
CREATE TABLE community_invitations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    community_id UUID NOT NULL REFERENCES communities (id) ON DELETE CASCADE,
    created_by UUID NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    token_hash CHAR(64) NOT NULL UNIQUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL,
    used_at TIMESTAMPTZ,
    used_by UUID REFERENCES users (id) ON DELETE SET NULL,
    CONSTRAINT invitation_lifetime CHECK (expires_at > created_at),
    CONSTRAINT invitation_used_after_creation CHECK (
        used_at IS NULL OR used_at >= created_at
    ),
    CONSTRAINT invitation_use_consistency CHECK (
        used_at IS NOT NULL OR used_by IS NULL
    )
);
CREATE INDEX community_invitations_active_idx
    ON community_invitations (community_id, expires_at)
    WHERE used_at IS NULL;
CREATE UNIQUE INDEX community_owner_membership_unique
    ON community_memberships (community_id) WHERE role = 'owner';
