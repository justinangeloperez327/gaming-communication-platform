//! Persistent, text-channel-only messaging. Every query and mutation checks
//! the authenticated user's community membership at the SQL boundary.
//! Message bodies are NOT sent to logs. Deleted text is overwritten in storage.

use axum::{
    Json,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Row, postgres::PgRow, types::Uuid};

use crate::{
    AppState,
    auth::{self, AuthError},
    realtime::{Event, EventKind},
};

#[derive(Deserialize)]
pub(crate) struct MessageInput {
    body: String,
}

#[derive(Deserialize, Default)]
pub(crate) struct PageQuery {
    limit: Option<i64>,
    before: Option<String>,
}

#[derive(Serialize)]
pub(crate) struct MessageView {
    id: String,
    channel_id: String,
    author_id: String,
    body: Option<String>,
    created_at_ms: i64,
    edited_at_ms: Option<i64>,
    deleted_at_ms: Option<i64>,
}

#[derive(Serialize)]
pub(crate) struct MessagePage {
    messages: Vec<MessageView>,
    next_before: Option<String>,
}

fn parse_id(raw: &str) -> Result<Uuid, AuthError> {
    Uuid::parse_str(raw).map_err(|_| AuthError::InvalidRequest)
}

fn valid_body(body: &str) -> bool {
    (1..=4000).contains(&body.len())
        && !body.trim().is_empty()
        && !body
            .chars()
            .any(|c| c == '\0' || (c.is_control() && c != '\n' && c != '\t'))
}

fn view(row: &PgRow) -> Result<MessageView, AuthError> {
    let id: Uuid = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    let channel: Uuid = row
        .try_get("channel_id")
        .map_err(|_| AuthError::Unavailable)?;
    let author: Uuid = row
        .try_get("author_id")
        .map_err(|_| AuthError::Unavailable)?;
    Ok(MessageView {
        id: id.to_string(),
        channel_id: channel.to_string(),
        author_id: author.to_string(),
        body: row.try_get("body").map_err(|_| AuthError::Unavailable)?,
        created_at_ms: row
            .try_get("created_at_ms")
            .map_err(|_| AuthError::Unavailable)?,
        edited_at_ms: row
            .try_get("edited_at_ms")
            .map_err(|_| AuthError::Unavailable)?,
        deleted_at_ms: row
            .try_get("deleted_at_ms")
            .map_err(|_| AuthError::Unavailable)?,
    })
}

async fn actor(state: &AppState, headers: &HeaderMap) -> Result<(PgPool, Uuid), AuthError> {
    let pool = auth::database(state)?;
    let (id, _) = auth::authenticated_user(pool, headers).await?;
    Ok((pool.clone(), id))
}

pub(crate) async fn text_channel(
    pool: &PgPool,
    community: Uuid,
    channel: Uuid,
    user: Uuid,
) -> Result<(), AuthError> {
    let allowed = sqlx::query_scalar::<_, bool>(
        "SELECT EXISTS (
           SELECT 1 FROM channels ch
           JOIN community_memberships cm ON cm.community_id = ch.community_id
           WHERE ch.id = $1 AND ch.community_id = $2
             AND ch.channel_kind = 'text' AND cm.user_id = $3
         )",
    )
    .bind(channel)
    .bind(community)
    .bind(user)
    .fetch_one(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if allowed {
        Ok(())
    } else {
        // Hide nonmember resources and voice-channel message operations.
        Err(AuthError::NotFound)
    }
}

pub(crate) async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
    Json(input): Json<MessageInput>,
) -> Result<(StatusCode, Json<MessageView>), AuthError> {
    if !valid_body(&input.body) {
        return Err(AuthError::InvalidRequest);
    }
    let community = parse_id(&community_id)?;
    let channel = parse_id(&channel_id)?;
    let (pool, user) = actor(&state, &headers).await?;
    // Authorization is inside INSERT...SELECT: an external channel ID
    // cannot inject content into an unrelated community or voice channel.
    let row = sqlx::query(
        "INSERT INTO messages (channel_id, author_id, body)
         SELECT ch.id, $3, $4 FROM channels ch
         WHERE ch.id = $1 AND ch.community_id = $2
           AND ch.channel_kind = 'text'
           AND EXISTS (
             SELECT 1 FROM community_memberships cm
             WHERE cm.community_id = ch.community_id AND cm.user_id = $3
           )
         RETURNING id, channel_id, author_id, body,
           floor(extract(epoch from created_at) * 1000)::bigint AS created_at_ms,
           NULL::bigint AS edited_at_ms, NULL::bigint AS deleted_at_ms",
    )
    .bind(channel)
    .bind(community)
    .bind(user)
    .bind(input.body)
    .fetch_optional(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::NotFound)?;
    let result = view(&row)?;
    state.realtime.publish(Event::message(
        channel,
        user,
        parse_id(&result.id)?,
        EventKind::Created,
    ));
    Ok((StatusCode::CREATED, Json(result)))
}

pub(crate) async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
    Query(query): Query<PageQuery>,
) -> Result<Json<MessagePage>, AuthError> {
    let community = parse_id(&community_id)?;
    let channel = parse_id(&channel_id)?;
    let size = query.limit.unwrap_or(50);
    if !(1..=100).contains(&size) {
        return Err(AuthError::InvalidRequest);
    }
    let before = query.before.as_deref().map(parse_id).transpose()?;
    let (pool, user) = actor(&state, &headers).await?;
    text_channel(&pool, community, channel, user).await?;
    let rows = sqlx::query(
        "SELECT m.id, m.channel_id, m.author_id,
           CASE WHEN m.deleted_at IS NULL THEN m.body ELSE NULL END AS body,
           floor(extract(epoch from m.created_at) * 1000)::bigint AS created_at_ms,
           floor(extract(epoch from m.edited_at) * 1000)::bigint AS edited_at_ms,
           floor(extract(epoch from m.deleted_at) * 1000)::bigint AS deleted_at_ms
         FROM messages m
         JOIN channels ch ON ch.id = m.channel_id
         JOIN community_memberships cm ON cm.community_id = ch.community_id
         WHERE m.channel_id = $1 AND ch.community_id = $2
           AND ch.channel_kind = 'text' AND cm.user_id = $3
           AND ($4::uuid IS NULL OR
             (m.created_at, m.id) < (
               SELECT cursor.created_at, cursor.id FROM messages cursor
               WHERE cursor.id = $4 AND cursor.channel_id = $1
             ))
         ORDER BY m.created_at DESC, m.id DESC LIMIT $5",
    )
    .bind(channel)
    .bind(community)
    .bind(user)
    .bind(before)
    .bind(size + 1)
    .fetch_all(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    let more = rows.len() as i64 > size;
    let mut messages = rows
        .iter()
        .take(size as usize)
        .map(view)
        .collect::<Result<Vec<_>, _>>()?;
    let next_before = if more {
        messages.last().map(|message| message.id.clone())
    } else {
        None
    };
    // Delete records are retained only as content-free tombstones; editing
    // and further deleting them is forbidden.
    Ok(Json(MessagePage {
        messages: std::mem::take(&mut messages),
        next_before,
    }))
}

pub(crate) async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id, message_id)): Path<(String, String, String)>,
) -> Result<Json<MessageView>, AuthError> {
    let community = parse_id(&community_id)?;
    let channel = parse_id(&channel_id)?;
    let message = parse_id(&message_id)?;
    let (pool, user) = actor(&state, &headers).await?;
    let row = sqlx::query(
        "SELECT m.id, m.channel_id, m.author_id,
           CASE WHEN m.deleted_at IS NULL THEN m.body ELSE NULL END AS body,
           floor(extract(epoch from m.created_at) * 1000)::bigint AS created_at_ms,
           floor(extract(epoch from m.edited_at) * 1000)::bigint AS edited_at_ms,
           floor(extract(epoch from m.deleted_at) * 1000)::bigint AS deleted_at_ms
         FROM messages m
         JOIN channels ch ON ch.id = m.channel_id
         JOIN community_memberships cm ON cm.community_id = ch.community_id
         WHERE m.id = $1 AND m.channel_id = $2 AND ch.community_id = $3
           AND ch.channel_kind = 'text' AND cm.user_id = $4",
    )
    .bind(message)
    .bind(channel)
    .bind(community)
    .bind(user)
    .fetch_optional(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::NotFound)?;
    Ok(Json(view(&row)?))
}

pub(crate) async fn edit(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id, message_id)): Path<(String, String, String)>,
    Json(input): Json<MessageInput>,
) -> Result<Json<MessageView>, AuthError> {
    if !valid_body(&input.body) {
        return Err(AuthError::InvalidRequest);
    }
    let community = parse_id(&community_id)?;
    let channel = parse_id(&channel_id)?;
    let message = parse_id(&message_id)?;
    let (pool, user) = actor(&state, &headers).await?;
    // Only the actual author may edit; no manager can silently change
    // someone else's words. Membership must still be active at mutation time.
    let row = sqlx::query(
        "UPDATE messages m SET body = $1, edited_at = now()
         WHERE m.id = $2 AND m.channel_id = $3 AND m.author_id = $4
           AND m.deleted_at IS NULL
           AND EXISTS (
             SELECT 1 FROM channels ch
             JOIN community_memberships cm ON cm.community_id = ch.community_id
             WHERE ch.id = m.channel_id AND ch.community_id = $5
               AND ch.channel_kind = 'text' AND cm.user_id = $4
           )
         RETURNING id, channel_id, author_id, body,
           floor(extract(epoch from created_at) * 1000)::bigint AS created_at_ms,
           floor(extract(epoch from edited_at) * 1000)::bigint AS edited_at_ms,
           NULL::bigint AS deleted_at_ms",
    )
    .bind(input.body)
    .bind(message)
    .bind(channel)
    .bind(user)
    .bind(community)
    .fetch_optional(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::NotFound)?;
    let result = view(&row)?;
    state.realtime.publish(Event::message(
        channel,
        user,
        parse_id(&result.id)?,
        EventKind::Edited,
    ));
    Ok(Json(result))
}

pub(crate) async fn delete(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id, message_id)): Path<(String, String, String)>,
) -> Result<StatusCode, AuthError> {
    let community = parse_id(&community_id)?;
    let channel = parse_id(&channel_id)?;
    let message = parse_id(&message_id)?;
    let (pool, user) = actor(&state, &headers).await?;
    // Moderators (owner/admin) can remove a member's post but cannot edit it.
    // Overwrite text in the DB rather than leaving the removed content there.
    let result = sqlx::query(
        "UPDATE messages m SET body = '[deleted]', deleted_at = now()
         WHERE m.id = $1 AND m.channel_id = $2 AND m.deleted_at IS NULL
           AND EXISTS (
             SELECT 1 FROM channels ch
             JOIN community_memberships cm ON cm.community_id = ch.community_id
             WHERE ch.id = m.channel_id AND ch.community_id = $3
               AND ch.channel_kind = 'text' AND cm.user_id = $4
               AND (m.author_id = $4 OR cm.role IN ('owner', 'admin'))
           )",
    )
    .bind(message)
    .bind(channel)
    .bind(community)
    .bind(user)
    .execute(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    state
        .realtime
        .publish(Event::message(channel, user, message, EventKind::Deleted));
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bound_message_sizes_and_cursor_validation() {
        assert!(valid_body("message"));
        assert!(valid_body("hi\nthere"));
        assert!(!valid_body(""));
        assert!(!valid_body(" \t\n"));
        assert!(!valid_body("bad\0content"));
        assert!(!valid_body(&"a".repeat(4001)));
        assert!(parse_id("invalid-id").is_err());
    }
}
