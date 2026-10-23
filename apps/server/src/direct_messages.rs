//! Friend-only private messages. Pair locks serialize all writes with block and
//! unfriend transitions. HTTP is authoritative; socket events contain IDs only.
use crate::{
    AppState,
    auth::AuthError,
    friends,
    messages::valid_body as message_body,
    realtime::{Event, EventKind},
};
use axum::{
    Json,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Postgres, Row, Transaction, types::Uuid};

fn error(_: sqlx::Error) -> AuthError {
    AuthError::Unavailable
}
fn uid(s: &str) -> Result<Uuid, AuthError> {
    Uuid::parse_str(s).map_err(|_| AuthError::InvalidRequest)
}
#[derive(Deserialize)]
pub(crate) struct OpenInput {
    user_id: String,
}
#[derive(Deserialize)]
pub(crate) struct BodyInput {
    body: String,
}
#[derive(Deserialize)]
pub(crate) struct CreateInput {
    body: String,
    client_message_id: Option<String>,
}
#[derive(Deserialize)]
pub(crate) struct Page {
    limit: Option<i64>,
    before: Option<String>,
}
#[derive(Serialize)]
pub(crate) struct Conversation {
    id: String,
    other_user_id: String,
}
#[derive(Serialize)]
pub(crate) struct DirectMessage {
    id: String,
    conversation_id: String,
    author_id: String,
    body: Option<String>,
    created_at_ms: i64,
    created_at_us: i64,
    edited_at_ms: Option<i64>,
    deleted_at_ms: Option<i64>,
}
#[derive(Serialize)]
pub(crate) struct DirectPage {
    messages: Vec<DirectMessage>,
    next_before: Option<String>,
}
fn conversation(row: &sqlx::postgres::PgRow) -> Result<Conversation, AuthError> {
    Ok(Conversation {
        id: row.try_get::<Uuid, _>("id").map_err(error)?.to_string(),
        other_user_id: row
            .try_get::<Uuid, _>("other_user_id")
            .map_err(error)?
            .to_string(),
    })
}
fn message(row: &sqlx::postgres::PgRow) -> Result<DirectMessage, AuthError> {
    Ok(DirectMessage {
        id: row.try_get::<Uuid, _>("id").map_err(error)?.to_string(),
        conversation_id: row
            .try_get::<Uuid, _>("conversation_id")
            .map_err(error)?
            .to_string(),
        author_id: row
            .try_get::<Uuid, _>("author_id")
            .map_err(error)?
            .to_string(),
        body: row.try_get("body").map_err(error)?,
        created_at_ms: row.try_get("created_at_ms").map_err(error)?,
        created_at_us: row.try_get("created_at_us").map_err(error)?,
        edited_at_ms: row.try_get("edited_at_ms").map_err(error)?,
        deleted_at_ms: row.try_get("deleted_at_ms").map_err(error)?,
    })
}
const FIELDS: &str = "m.id,m.conversation_id,m.author_id,
    CASE WHEN m.deleted_at IS NULL THEN m.body ELSE NULL END AS body,
    floor(extract(epoch from m.created_at)*1000)::bigint AS created_at_ms,
    floor(extract(epoch from m.created_at)*1000000)::bigint AS created_at_us,
    floor(extract(epoch from m.edited_at)*1000)::bigint AS edited_at_ms,
    floor(extract(epoch from m.deleted_at)*1000)::bigint AS deleted_at_ms";
// $1 is always the conversation, $2 is always the authenticated caller.
const ACCESS: &str = "EXISTS (
    SELECT 1 FROM dm_conversations c
    JOIN friendships f ON f.user_low=c.user_low AND f.user_high=c.user_high
    WHERE c.id=$1 AND ($2=c.user_low OR $2=c.user_high)
      AND NOT EXISTS (SELECT 1 FROM user_blocks b WHERE
        (b.blocker_id=c.user_low AND b.blocked_id=c.user_high)
        OR (b.blocker_id=c.user_high AND b.blocked_id=c.user_low)))";
pub(crate) async fn authorize(pool: &PgPool, cid: Uuid, user: Uuid) -> Result<(), AuthError> {
    let yes: bool = sqlx::query_scalar(&format!("SELECT {ACCESS}"))
        .bind(cid)
        .bind(user)
        .fetch_one(pool)
        .await
        .map_err(error)?;
    if yes {
        Ok(())
    } else {
        Err(AuthError::NotFound)
    }
}
async fn lock_conversation(
    pool: &PgPool,
    cid: Uuid,
    user: Uuid,
) -> Result<Transaction<'_, Postgres>, AuthError> {
    let pair = sqlx::query("SELECT user_low,user_high FROM dm_conversations WHERE id=$1 AND (user_low=$2 OR user_high=$2)")
        .bind(cid).bind(user).fetch_optional(pool).await.map_err(error)?.ok_or(AuthError::NotFound)?;
    let low: Uuid = pair.try_get("user_low").map_err(error)?;
    let high: Uuid = pair.try_get("user_high").map_err(error)?;
    let mut tx = pool.begin().await.map_err(error)?;
    friends::lock_pair(&mut tx, low, high).await?;
    Ok(tx)
}
pub(crate) async fn open(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<OpenInput>,
) -> Result<(StatusCode, Json<Conversation>), AuthError> {
    let (pool, user) = friends::actor(&state, &headers).await?;
    let other = uid(&input.user_id)?;
    let mut tx = pool.begin().await.map_err(error)?;
    friends::lock_pair(&mut tx, user, other).await?;
    let row = sqlx::query("INSERT INTO dm_conversations(user_low,user_high)
        SELECT LEAST($1::uuid,$2::uuid),GREATEST($1::uuid,$2::uuid)
        WHERE EXISTS (SELECT 1 FROM friendships WHERE user_low=LEAST($1::uuid,$2::uuid) AND user_high=GREATEST($1::uuid,$2::uuid))
          AND NOT EXISTS (SELECT 1 FROM user_blocks WHERE (blocker_id=$1 AND blocked_id=$2) OR (blocker_id=$2 AND blocked_id=$1))
        ON CONFLICT(user_low,user_high) DO UPDATE SET user_low=EXCLUDED.user_low
        RETURNING id, CASE WHEN user_low=$1 THEN user_high ELSE user_low END AS other_user_id")
        .bind(user).bind(other).fetch_optional(&mut *tx).await.map_err(error)?.ok_or(AuthError::NotFound)?;
    let result = conversation(&row)?;
    tx.commit().await.map_err(error)?;
    Ok((StatusCode::OK, Json(result)))
}
pub(crate) async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<Conversation>>, AuthError> {
    let (pool, user) = friends::actor(&state, &headers).await?;
    let rows = sqlx::query("SELECT c.id, CASE WHEN c.user_low=$1 THEN c.user_high ELSE c.user_low END AS other_user_id
        FROM dm_conversations c JOIN friendships f ON f.user_low=c.user_low AND f.user_high=c.user_high
        WHERE (c.user_low=$1 OR c.user_high=$1) AND NOT EXISTS (SELECT 1 FROM user_blocks b WHERE
            (b.blocker_id=c.user_low AND b.blocked_id=c.user_high) OR (b.blocker_id=c.user_high AND b.blocked_id=c.user_low))
        ORDER BY c.created_at DESC,c.id DESC LIMIT 100")
        .bind(user).fetch_all(&pool).await.map_err(error)?;
    Ok(Json(
        rows.iter().map(conversation).collect::<Result<_, _>>()?,
    ))
}
pub(crate) async fn history(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(raw): Path<String>,
    Query(page): Query<Page>,
) -> Result<Json<DirectPage>, AuthError> {
    let cid = uid(&raw)?;
    let limit = page.limit.unwrap_or(50);
    if !(1..=100).contains(&limit) {
        return Err(AuthError::InvalidRequest);
    }
    let before = page.before.as_deref().map(uid).transpose()?;
    let (pool, user) = friends::actor(&state, &headers).await?;
    authorize(&pool, cid, user).await?;
    let sql = format!("SELECT {FIELDS} FROM dm_messages m WHERE m.conversation_id=$1 AND {ACCESS}
        AND ($3::uuid IS NULL OR (m.created_at,m.id) < (SELECT cursor.created_at,cursor.id FROM dm_messages cursor
            WHERE cursor.id=$3 AND cursor.conversation_id=$1)) ORDER BY m.created_at DESC,m.id DESC LIMIT $4");
    let rows = sqlx::query(&sql)
        .bind(cid)
        .bind(user)
        .bind(before)
        .bind(limit + 1)
        .fetch_all(&pool)
        .await
        .map_err(error)?;
    let more = rows.len() as i64 > limit;
    let messages = rows
        .iter()
        .take(limit as usize)
        .map(message)
        .collect::<Result<Vec<_>, _>>()?;
    let next_before = if more {
        messages.last().map(|m| m.id.clone())
    } else {
        None
    };
    Ok(Json(DirectPage {
        messages,
        next_before,
    }))
}
pub(crate) async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((raw, mid)): Path<(String, String)>,
) -> Result<Json<DirectMessage>, AuthError> {
    let cid = uid(&raw)?;
    let mid = uid(&mid)?;
    let (pool, user) = friends::actor(&state, &headers).await?;
    let row = sqlx::query(&format!(
        "SELECT {FIELDS} FROM dm_messages m WHERE m.conversation_id=$1 AND m.id=$3 AND {ACCESS}"
    ))
    .bind(cid)
    .bind(user)
    .bind(mid)
    .fetch_optional(&pool)
    .await
    .map_err(error)?
    .ok_or(AuthError::NotFound)?;
    Ok(Json(message(&row)?))
}
pub(crate) async fn send(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(raw): Path<String>,
    Json(input): Json<CreateInput>,
) -> Result<(StatusCode, Json<DirectMessage>), AuthError> {
    if !message_body(&input.body) {
        return Err(AuthError::InvalidRequest);
    }
    let cid = uid(&raw)?;
    let attempt = input.client_message_id.as_deref().map(uid).transpose()?;
    if attempt.is_some_and(|id| id.is_nil()) {
        return Err(AuthError::InvalidRequest);
    }
    let (pool, user) = friends::actor(&state, &headers).await?;
    let mut tx = lock_conversation(&pool, cid, user).await?;
    let sql = format!(
        "INSERT INTO dm_messages(id,conversation_id,author_id,body)
        SELECT COALESCE($4::uuid,gen_random_uuid()),$1,$2,$3 WHERE {ACCESS}
        ON CONFLICT(id) DO NOTHING RETURNING id,conversation_id,author_id,body,
        floor(extract(epoch from created_at)*1000)::bigint AS created_at_ms,
        floor(extract(epoch from created_at)*1000000)::bigint AS created_at_us,
        NULL::bigint AS edited_at_ms,NULL::bigint AS deleted_at_ms"
    );
    let row = sqlx::query(&sql)
        .bind(cid)
        .bind(user)
        .bind(input.body)
        .bind(attempt)
        .fetch_optional(&mut *tx)
        .await
        .map_err(error)?;
    let created = row.is_some();
    let row = match row {
        Some(row) => row,
        None => {
            let id = attempt.ok_or(AuthError::NotFound)?;
            sqlx::query(&format!("SELECT {FIELDS} FROM dm_messages m WHERE m.conversation_id=$1 AND m.author_id=$2 AND m.id=$3 AND {ACCESS}"))
                .bind(cid).bind(user).bind(id).fetch_optional(&mut *tx).await.map_err(error)?.ok_or(AuthError::NotFound)?
        }
    };
    let result = message(&row)?;
    tx.commit().await.map_err(error)?;
    if created {
        state.dm_realtime.publish(Event::message(
            cid,
            user,
            uid(&result.id)?,
            EventKind::Created,
        ));
    }
    Ok((
        if created {
            StatusCode::CREATED
        } else {
            StatusCode::OK
        },
        Json(result),
    ))
}
pub(crate) async fn edit(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((raw, mid)): Path<(String, String)>,
    Json(input): Json<BodyInput>,
) -> Result<Json<DirectMessage>, AuthError> {
    if !message_body(&input.body) {
        return Err(AuthError::InvalidRequest);
    }
    let cid = uid(&raw)?;
    let mid = uid(&mid)?;
    let (pool, user) = friends::actor(&state, &headers).await?;
    let mut tx = lock_conversation(&pool, cid, user).await?;
    let sql = format!("UPDATE dm_messages m SET body=$4,edited_at=greatest(clock_timestamp(),m.created_at,m.edited_at)
        WHERE m.conversation_id=$1 AND m.author_id=$2 AND m.id=$3 AND m.deleted_at IS NULL AND {ACCESS}
        RETURNING {FIELDS}");
    let row = sqlx::query(&sql)
        .bind(cid)
        .bind(user)
        .bind(mid)
        .bind(input.body)
        .fetch_optional(&mut *tx)
        .await
        .map_err(error)?
        .ok_or(AuthError::NotFound)?;
    let result = message(&row)?;
    tx.commit().await.map_err(error)?;
    state
        .dm_realtime
        .publish(Event::message(cid, user, mid, EventKind::Edited));
    Ok(Json(result))
}
pub(crate) async fn delete(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((raw, mid)): Path<(String, String)>,
) -> Result<StatusCode, AuthError> {
    let cid = uid(&raw)?;
    let mid = uid(&mid)?;
    let (pool, user) = friends::actor(&state, &headers).await?;
    let mut tx = lock_conversation(&pool, cid, user).await?;
    let sql = format!("UPDATE dm_messages m SET body='[deleted]',deleted_at=greatest(clock_timestamp(),m.created_at,m.edited_at)
        WHERE m.conversation_id=$1 AND m.author_id=$2 AND m.id=$3 AND m.deleted_at IS NULL AND {ACCESS}");
    let changed = sqlx::query(&sql)
        .bind(cid)
        .bind(user)
        .bind(mid)
        .execute(&mut *tx)
        .await
        .map_err(error)?;
    if changed.rows_affected() != 1 {
        return Err(AuthError::NotFound);
    }
    tx.commit().await.map_err(error)?;
    state
        .dm_realtime
        .publish(Event::message(cid, user, mid, EventKind::Deleted));
    Ok(StatusCode::NO_CONTENT)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validate_dm_content() {
        assert!(message_body("hello"));
        assert!(!message_body(""));
        assert!(!message_body(" \n"));
        assert!(!message_body(&"x".repeat(4001)));
    }
}
