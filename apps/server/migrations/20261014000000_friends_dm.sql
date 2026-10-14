-- Group 25: directed friend requests, undirected friendships, directional blocks
-- and private direct-message conversations. APIs always enforce access controls.
CREATE TABLE friend_requests (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    sender_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    receiver_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status TEXT NOT NULL DEFAULT 'pending'
        CHECK (status IN ('pending', 'accepted', 'declined')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    resolved_at TIMESTAMPTZ,
    CHECK (sender_id <> receiver_id),
    CHECK ((status = 'pending') = (resolved_at IS NULL))
);
CREATE UNIQUE INDEX friend_requests_one_pending_pair
    ON friend_requests (LEAST(sender_id, receiver_id), GREATEST(sender_id, receiver_id))
    WHERE status = 'pending';
CREATE INDEX friend_requests_incoming_idx
    ON friend_requests (receiver_id, created_at DESC) WHERE status = 'pending';
CREATE INDEX friend_requests_outgoing_idx
    ON friend_requests (sender_id, created_at DESC) WHERE status = 'pending';

CREATE TABLE friendships (
    user_low UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_high UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_low, user_high),
    CHECK (user_low < user_high)
);
CREATE INDEX friendships_high_idx ON friendships(user_high, user_low);

CREATE TABLE user_blocks (
    blocker_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    blocked_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (blocker_id, blocked_id),
    CHECK (blocker_id <> blocked_id)
);
CREATE INDEX user_blocks_blocked_idx ON user_blocks(blocked_id);

CREATE TABLE dm_conversations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_low UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_high UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE(user_low, user_high),
    CHECK (user_low < user_high)
);
CREATE INDEX dm_conversations_high_idx ON dm_conversations(user_high);

CREATE TABLE dm_messages (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    conversation_id UUID NOT NULL REFERENCES dm_conversations(id) ON DELETE CASCADE,
    author_id UUID NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    body TEXT NOT NULL CHECK (octet_length(body) BETWEEN 1 AND 4000),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    edited_at TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,
    CHECK (edited_at IS NULL OR edited_at >= created_at),
    CHECK (deleted_at IS NULL OR deleted_at >= created_at)
);
CREATE INDEX dm_messages_page_idx
    ON dm_messages(conversation_id, created_at DESC, id DESC);
