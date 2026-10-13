-- GCP core persistence schema. PostgreSQL 15+; no extensions required.
-- This migration defines durable entities only. Authenticated API handlers
-- and runtime authorization checks are intentionally added in later groups.

CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    handle TEXT NOT NULL UNIQUE
        CHECK (handle ~ '^[a-z0-9_]{3,32}$'),
    display_name TEXT NOT NULL
        CHECK (char_length(btrim(display_name)) BETWEEN 1 AND 80),
    email TEXT NOT NULL
        CHECK (char_length(email) BETWEEN 3 AND 320),
    password_hash TEXT NOT NULL
        CHECK (char_length(password_hash) BETWEEN 20 AND 512),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Email equality is case-insensitive without requiring CITEXT extension.
CREATE UNIQUE INDEX users_email_lower_unique ON users ((lower(email)));

CREATE TABLE communities (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL
        CHECK (char_length(btrim(name)) BETWEEN 1 AND 100),
    owner_id UUID NOT NULL REFERENCES users (id) ON DELETE RESTRICT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX communities_owner_id_index ON communities (owner_id);

CREATE TABLE community_memberships (
    community_id UUID NOT NULL REFERENCES communities (id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    role TEXT NOT NULL DEFAULT 'member'
        CHECK (role IN ('owner', 'admin', 'member')),
    joined_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (community_id, user_id)
);

CREATE INDEX community_memberships_user_id_index
    ON community_memberships (user_id, community_id);

CREATE TABLE channels (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    community_id UUID NOT NULL REFERENCES communities (id) ON DELETE CASCADE,
    name TEXT NOT NULL
        CHECK (char_length(btrim(name)) BETWEEN 1 AND 80),
    channel_kind TEXT NOT NULL CHECK (channel_kind IN ('text', 'voice')),
    position INTEGER NOT NULL DEFAULT 0 CHECK (position >= 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (id, community_id)
);

CREATE UNIQUE INDEX channels_community_name_unique
    ON channels (community_id, lower(name));
CREATE INDEX channels_community_order_index
    ON channels (community_id, position, id);

CREATE TABLE messages (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    channel_id UUID NOT NULL REFERENCES channels (id) ON DELETE CASCADE,
    author_id UUID NOT NULL REFERENCES users (id) ON DELETE RESTRICT,
    body TEXT NOT NULL CHECK (char_length(body) BETWEEN 1 AND 4000),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    edited_at TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,
    CONSTRAINT edited_not_before_created
        CHECK (edited_at IS NULL OR edited_at >= created_at),
    CONSTRAINT deleted_not_before_created
        CHECK (deleted_at IS NULL OR deleted_at >= created_at)
);

CREATE INDEX messages_channel_page_index
    ON messages (channel_id, created_at DESC, id DESC);
CREATE INDEX messages_author_index ON messages (author_id, created_at DESC);

-- Future APIs must verify authenticated membership and text-channel type.
-- Foreign keys enforce referential integrity, NOT app-level authorization.
