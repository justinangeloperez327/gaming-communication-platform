//! Bounded, database-backed authorization leases, not proof of OS capture consent.
//! Native capture/view controls are implemented separately in roadmap Groups 8/9.
use axum::{
    Json,
    extract::{Path, State},
    http::{HeaderMap, StatusCode, header},
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Postgres, Transaction, types::Uuid};

use crate::{
    AppState,
    auth::{self, AuthError},
    livekit_tokens::LiveKitConfig,
};

pub(crate) const LEASE_SECONDS: u64 = 60;

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub(crate) enum Purpose {
    Publish,
    View,
}

impl Purpose {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Publish => "publish",
            Self::View => "view",
        }
    }
}

pub(crate) fn room_name(channel: Uuid) -> String {
    format!("gcp-screen-{channel}")
}

pub(crate) fn identity(lease: Uuid, purpose: Purpose) -> String {
    format!("gcp-screen-{}-{lease}", purpose.as_str())
}

pub(crate) fn parse_identity(raw: &str) -> Option<(Uuid, Purpose)> {
    for purpose in [Purpose::Publish, Purpose::View] {
        let prefix = format!("gcp-screen-{}-", purpose.as_str());
        if let Some(id) = raw.strip_prefix(&prefix) {
            let id = Uuid::parse_str(id).ok()?;
            return (identity(id, purpose) == raw).then_some((id, purpose));
        }
    }
    None
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Create {
    purpose: Purpose,
    consent: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Renew {
    consent: bool,
}

#[derive(Serialize)]
pub(crate) struct Grant {
    id: String,
    purpose: Purpose,
    participant_identity: String,
    #[serde(flatten)]
    token: crate::livekit_tokens::TokenResponse,
}

#[derive(Serialize, sqlx::FromRow)]
pub(crate) struct Publisher {
    id: String,
    user_id: String,
    display_name: String,
    participant_identity: String,
}

fn ids(community: &str, channel: &str) -> Result<(Uuid, Uuid), AuthError> {
    Ok((
        Uuid::parse_str(community).map_err(|_| AuthError::InvalidRequest)?,
        Uuid::parse_str(channel).map_err(|_| AuthError::InvalidRequest)?,
    ))
}

// Serialize create/renew with logout and one another. Recheck after acquiring
// locks: a request can wait past session expiry or access-token rotation.
async fn locked_access<'a>(
    pool: &'a PgPool,
    session: Uuid,
    digest: &str,
    community: Uuid,
    channel: Uuid,
) -> Result<Transaction<'a, Postgres>, AuthError> {
    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    let user: Option<Uuid> = sqlx::query_scalar(
        "SELECT user_id FROM user_sessions WHERE id=$1 AND access_hash=$2
         AND revoked_at IS NULL AND access_expires_at>clock_timestamp()
         AND refresh_expires_at>clock_timestamp() FOR UPDATE",
    )
    .bind(session)
    .bind(digest)
    .fetch_optional(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    let user = user.ok_or(AuthError::Unauthorized)?;
    let found: Option<Uuid> = sqlx::query_scalar(
        "SELECT c.id FROM channels c
         JOIN community_memberships m ON m.community_id=c.community_id
         WHERE c.id=$1 AND c.community_id=$2 AND c.channel_kind='voice'
         AND m.user_id=$3 FOR SHARE OF c,m",
    )
    .bind(channel)
    .bind(community)
    .bind(user)
    .fetch_optional(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    found.ok_or(AuthError::NotFound)?;
    let current: bool = sqlx::query_scalar(
        "SELECT EXISTS(SELECT 1 FROM user_sessions WHERE id=$1
         AND access_expires_at>clock_timestamp() AND refresh_expires_at>clock_timestamp())",
    )
    .bind(session)
    .fetch_one(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if !current {
        return Err(AuthError::Unauthorized);
    }
    Ok(tx)
}

fn config(state: &AppState) -> Result<&LiveKitConfig, AuthError> {
    let config = state.livekit.as_ref().ok_or(AuthError::Unavailable)?;
    if !config.eviction_healthy() {
        return Err(AuthError::Unavailable);
    }
    Ok(config)
}

type GrantReply = (
    StatusCode,
    [(header::HeaderName, &'static str); 2],
    Json<Grant>,
);

fn reply(
    config: &LiveKitConfig,
    id: Uuid,
    channel: Uuid,
    purpose: Purpose,
    expiry: i64,
    status: StatusCode,
) -> Result<GrantReply, AuthError> {
    let expiry = u64::try_from(expiry).map_err(|_| AuthError::Unavailable)?;
    Ok((
        status,
        [
            (header::CACHE_CONTROL, "no-store"),
            (header::PRAGMA, "no-cache"),
        ],
        Json(Grant {
            id: id.to_string(),
            purpose,
            participant_identity: identity(id, purpose),
            token: config.screen_token(id, channel, purpose, expiry)?,
        }),
    ))
}

pub(crate) async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community, channel)): Path<(String, String)>,
    Json(input): Json<Create>,
) -> Result<GrantReply, AuthError> {
    let pool = auth::database(&state)?;
    let (_, session, _) = auth::authenticated_session(pool, &headers).await?;
    if !input.consent {
        return Err(AuthError::InvalidRequest);
    }
    let (community, channel) = ids(&community, &channel)?;
    let digest = auth::bearer_digest(&headers)?;
    let mut tx = locked_access(pool, session, &digest, community, channel).await?;
    let config = config(&state)?;
    sqlx::query("DELETE FROM screen_leases WHERE session_id=$1 AND expires_at<=clock_timestamp()")
        .bind(session)
        .execute(&mut *tx)
        .await
        .map_err(|_| AuthError::Unavailable)?;
    let row: Option<(Uuid, i64)> = sqlx::query_as(
        "INSERT INTO screen_leases(session_id,channel_id,purpose,expires_at)
         VALUES($1,$2,$3,clock_timestamp()+interval '60 seconds')
         ON CONFLICT(session_id,purpose) DO NOTHING
         RETURNING id,floor(extract(epoch FROM expires_at))::bigint",
    )
    .bind(session)
    .bind(channel)
    .bind(input.purpose.as_str())
    .fetch_optional(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    let (id, expiry) = row.ok_or(AuthError::Conflict)?;
    let result = reply(
        config,
        id,
        channel,
        input.purpose,
        expiry,
        StatusCode::CREATED,
    )?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok(result)
}

pub(crate) async fn renew(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community, channel, lease)): Path<(String, String, String)>,
    Json(input): Json<Renew>,
) -> Result<GrantReply, AuthError> {
    let pool = auth::database(&state)?;
    let (_, session, _) = auth::authenticated_session(pool, &headers).await?;
    if !input.consent {
        return Err(AuthError::InvalidRequest);
    }
    let (community, channel) = ids(&community, &channel)?;
    let lease = Uuid::parse_str(&lease).map_err(|_| AuthError::InvalidRequest)?;
    let digest = auth::bearer_digest(&headers)?;
    let mut tx = locked_access(pool, session, &digest, community, channel).await?;
    let config = config(&state)?;
    let row: Option<(String, i64)> = sqlx::query_as(
        "UPDATE screen_leases SET expires_at=clock_timestamp()+interval '60 seconds'
         WHERE id=$1 AND session_id=$2 AND channel_id=$3 AND expires_at>clock_timestamp()
         RETURNING purpose,floor(extract(epoch FROM expires_at))::bigint",
    )
    .bind(lease)
    .bind(session)
    .bind(channel)
    .fetch_optional(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    let (purpose, expiry) = row.ok_or(AuthError::NotFound)?;
    let purpose = match purpose.as_str() {
        "publish" => Purpose::Publish,
        "view" => Purpose::View,
        _ => return Err(AuthError::Unavailable),
    };
    let result = reply(config, lease, channel, purpose, expiry, StatusCode::OK)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok(result)
}

pub(crate) async fn stop(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community, channel, lease)): Path<(String, String, String)>,
) -> Result<StatusCode, AuthError> {
    let pool = auth::database(&state)?;
    let (_, session, _) = auth::authenticated_session(pool, &headers).await?;
    let (community, channel) = ids(&community, &channel)?;
    let lease = Uuid::parse_str(&lease).map_err(|_| AuthError::InvalidRequest)?;
    // Stop remains available after membership loss or an SFU outage. Repeated,
    // absent and other-login IDs all return 204 without revealing ownership.
    let purpose: Option<String> = sqlx::query_scalar(
        "DELETE FROM screen_leases l USING channels c
         WHERE l.id=$1 AND l.session_id=$2 AND l.channel_id=$3
         AND c.id=l.channel_id AND c.community_id=$4 RETURNING l.purpose",
    )
    .bind(lease)
    .bind(session)
    .bind(channel)
    .bind(community)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if let (Some(config), Some(purpose)) = (state.livekit.as_ref(), purpose) {
        let purpose = if purpose == "publish" {
            Purpose::Publish
        } else {
            Purpose::View
        };
        // Lease invalidation has committed. No DB lock is held during HTTP;
        // reconciliation retries a failed immediate disconnect after restart.
        let _ = config
            .remove_screen_participant(channel, lease, purpose)
            .await;
    }
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn publishers(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community, channel)): Path<(String, String)>,
) -> Result<Json<Vec<Publisher>>, AuthError> {
    let pool = auth::database(&state)?;
    let (_, session, _) = auth::authenticated_session(pool, &headers).await?;
    let (community, channel) = ids(&community, &channel)?;
    let digest = auth::bearer_digest(&headers)?;
    let mut tx = locked_access(pool, session, &digest, community, channel).await?;
    let rows = sqlx::query_as::<_, Publisher>(
        "SELECT l.id::text,s.user_id::text,u.display_name,'gcp-screen-publish-'||l.id::text AS participant_identity
         FROM screen_leases l JOIN user_sessions s ON s.id=l.session_id
         JOIN users u ON u.id=s.user_id
         JOIN channels c ON c.id=l.channel_id
         JOIN community_memberships m ON m.community_id=c.community_id AND m.user_id=s.user_id
         WHERE l.channel_id=$1 AND l.purpose='publish' AND l.expires_at>clock_timestamp()
         AND s.revoked_at IS NULL AND s.access_expires_at>clock_timestamp()
         AND s.refresh_expires_at>clock_timestamp() ORDER BY l.id LIMIT 201")
        .bind(channel).fetch_all(&mut *tx).await.map_err(|_| AuthError::Unavailable)?;
    if rows.len() > 200 {
        return Err(AuthError::Busy);
    }
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok(Json(rows))
}


#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Watch {
    consent: bool,
    publisher_id: String,
}
#[derive(Serialize)]
pub(crate) struct WatchReply {
    participant_identity: String,
    track_sid: String,
}

async fn watch_access(
    pool: &PgPool, session: Uuid, digest: &str, community: Uuid, channel: Uuid,
    viewer: Uuid, publisher: Uuid,
) -> Result<(), AuthError> {
    let mut tx = locked_access(pool, session, digest, community, channel).await?;
    let valid: bool = sqlx::query_scalar(
        "SELECT EXISTS(SELECT 1 FROM screen_leases WHERE id=$1 AND session_id=$2
         AND channel_id=$3 AND purpose='view' AND expires_at>clock_timestamp())
         AND EXISTS(SELECT 1 FROM screen_leases l JOIN user_sessions s ON s.id=l.session_id
         JOIN community_memberships m ON m.user_id=s.user_id AND m.community_id=$4
         WHERE l.id=$5 AND l.channel_id=$3 AND l.purpose='publish' AND l.expires_at>clock_timestamp()
         AND s.revoked_at IS NULL AND s.access_expires_at>clock_timestamp()
         AND s.refresh_expires_at>clock_timestamp())")
        .bind(viewer).bind(session).bind(channel).bind(community).bind(publisher)
        .fetch_one(&mut *tx).await.map_err(|_| AuthError::Unavailable)?;
    if !valid { return Err(AuthError::NotFound); }
    tx.commit().await.map_err(|_| AuthError::Unavailable)
}

pub(crate) async fn watch(
    State(state): State<AppState>, headers: HeaderMap,
    Path((community, channel, lease)): Path<(String, String, String)>,
    Json(input): Json<Watch>,
) -> Result<Json<WatchReply>, AuthError> {
    let pool = auth::database(&state)?;
    let (_, session, _) = auth::authenticated_session(pool, &headers).await?;
    if !input.consent { return Err(AuthError::InvalidRequest); }
    let (community, channel) = ids(&community, &channel)?;
    let viewer = Uuid::parse_str(&lease).map_err(|_| AuthError::InvalidRequest)?;
    let publisher = Uuid::parse_str(&input.publisher_id).map_err(|_| AuthError::InvalidRequest)?;
    let digest = auth::bearer_digest(&headers)?;
    watch_access(pool, session, &digest, community, channel, viewer, publisher).await?;
    let config = config(&state)?;
    // No DB locks cross SFU I/O. Recheck ownership/access afterwards, and remove
    // the viewer if concurrent logout/revocation/expiry invalidated the request.
    let sid = config.watch_screen(channel, viewer, publisher).await.map_err(|_| AuthError::Unavailable)?;
    if let Err(error) = watch_access(pool, session, &digest, community, channel, viewer, publisher).await {
        let _ = config.remove_screen_participant(channel, viewer, Purpose::View).await;
        return Err(error);
    }
    Ok(Json(WatchReply { participant_identity: identity(publisher, Purpose::Publish), track_sid: sid }))
}
