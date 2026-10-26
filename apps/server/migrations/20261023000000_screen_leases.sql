-- Screen authorization is independent of the microphone login identity.
-- Only two rows per login (one publisher, one viewer); expired rows are replaced.
-- No monitor/window name, desktop title or captured content belongs here.
CREATE TABLE screen_leases (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id UUID NOT NULL REFERENCES user_sessions (id) ON DELETE CASCADE,
    channel_id UUID NOT NULL REFERENCES channels (id) ON DELETE CASCADE,
    purpose TEXT NOT NULL CHECK (purpose IN ('publish', 'view')),
    expires_at TIMESTAMPTZ NOT NULL,
    UNIQUE (session_id, purpose)
);
CREATE INDEX screen_leases_channel_idx ON screen_leases (channel_id, expires_at);
