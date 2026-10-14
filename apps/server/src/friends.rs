//! Authenticated friend requests, canonical friendships and directional blocks.
//! Pairwise ordered row locks serialize friend/block transitions. Identity is
//! always derived from the database-backed bearer session.

use axum::{
    Json,
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Postgres, Row, Transaction, types::Uuid};

use crate::{
    AppState,
    auth::{self, AuthError},
};

fn err(_: sqlx::Error) -> AuthError {
    AuthError::Unavailable
}
fn id(value: &str) -> Result<Uuid, AuthError> {
    Uuid::parse_str(value).map_err(|_| AuthError::InvalidRequest)
}

pub(crate) async fn actor(
    state: &AppState,
    headers: &HeaderMap,
) -> Result<(PgPool, Uuid), AuthError> {
    let pool = auth::database(state)?;
    let (user, _) = auth::authenticated_user(pool, headers).await?;
    Ok((pool.clone(), user))
}

pub(crate) async fn lock_pair(
    tx: &mut Transaction<'_, Postgres>,
    user: Uuid,
    other: Uuid,
) -> Result<(), AuthError> {
    if user == other {
        return Err(AuthError::InvalidRequest);
    }
    let locked = sqlx::query("SELECT id FROM users WHERE id IN ($1, $2) ORDER BY id FOR UPDATE")
        .bind(user)
        .bind(other)
        .fetch_all(&mut **tx)
        .await
        .map_err(err)?;
    if locked.len() != 2 {
        return Err(AuthError::NotFound);
    }
    Ok(())
}

#[derive(Deserialize)]
pub(crate) struct RequestFriend {
    handle: String,
}
#[derive(Deserialize)]
pub(crate) struct BlockUser {
    user_id: String,
}

#[derive(Serialize)]
pub(crate) struct FriendRequestView {
    id: String,
    sender_id: String,
    receiver_id: String,
}
#[derive(Serialize)]
pub(crate) struct UserLinkView {
    user_id: String,
    handle: String,
    display_name: String,
}

fn request_view(row: &sqlx::postgres::PgRow) -> Result<FriendRequestView, AuthError> {
    let rid: Uuid = row.try_get("id").map_err(err)?;
    let from: Uuid = row.try_get("sender_id").map_err(err)?;
    let to: Uuid = row.try_get("receiver_id").map_err(err)?;
    Ok(FriendRequestView {
        id: rid.to_string(),
        sender_id: from.to_string(),
        receiver_id: to.to_string(),
    })
}
fn user_view(row: &sqlx::postgres::PgRow) -> Result<UserLinkView, AuthError> {
    let user: Uuid = row.try_get("user_id").map_err(err)?;
    Ok(UserLinkView {
        user_id: user.to_string(),
        handle: row.try_get("handle").map_err(err)?,
        display_name: row.try_get("display_name").map_err(err)?,
    })
}

pub(crate) async fn send_request(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<RequestFriend>,
) -> Result<(StatusCode, Json<FriendRequestView>), AuthError> {
    if input.handle.len() < 3
        || input.handle.len() > 32
        || !input
            .handle
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
    {
        return Err(AuthError::InvalidRequest);
    }
    let (pool, user) = actor(&state, &headers).await?;
    let other: Uuid = sqlx::query_scalar("SELECT id FROM users WHERE handle = $1")
        .bind(input.handle)
        .fetch_optional(&pool)
        .await
        .map_err(err)?
        .ok_or(AuthError::NotFound)?;
    if user == other {
        return Err(AuthError::InvalidRequest);
    }
    let mut tx = pool.begin().await.map_err(err)?;
    lock_pair(&mut tx, user, other).await?;
    let row = sqlx::query(
        "INSERT INTO friend_requests(sender_id, receiver_id)
         SELECT $1, $2 WHERE NOT EXISTS (
           SELECT 1 FROM user_blocks
           WHERE (blocker_id = $1 AND blocked_id = $2)
              OR (blocker_id = $2 AND blocked_id = $1)
         ) AND NOT EXISTS (
           SELECT 1 FROM friendships
           WHERE user_low = LEAST($1::uuid,$2::uuid)
             AND user_high = GREATEST($1::uuid,$2::uuid)
         ) AND NOT EXISTS (
           SELECT 1 FROM friend_requests WHERE status='pending'
             AND LEAST(sender_id,receiver_id)=LEAST($1::uuid,$2::uuid)
             AND GREATEST(sender_id,receiver_id)=GREATEST($1::uuid,$2::uuid)
         )
         RETURNING id,sender_id,receiver_id",
    )
    .bind(user)
    .bind(other)
    .fetch_optional(&mut *tx)
    .await
    .map_err(err)?
    .ok_or(AuthError::Conflict)?;
    let result = request_view(&row)?;
    tx.commit().await.map_err(err)?;
    Ok((StatusCode::CREATED, Json(result)))
}

pub(crate) async fn requests(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<FriendRequestView>>, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let rows = sqlx::query(
        "SELECT id,sender_id,receiver_id FROM friend_requests
         WHERE status='pending' AND (sender_id=$1 OR receiver_id=$1)
         ORDER BY created_at DESC,id DESC LIMIT 100",
    )
    .bind(user)
    .fetch_all(&pool)
    .await
    .map_err(err)?;
    Ok(Json(
        rows.iter().map(request_view).collect::<Result<_, _>>()?,
    ))
}

async fn resolve(
    state: AppState,
    headers: HeaderMap,
    raw: String,
    accept: bool,
) -> Result<StatusCode, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let request = id(&raw)?;
    let sender: Uuid = sqlx::query_scalar(
        "SELECT sender_id FROM friend_requests
         WHERE id=$1 AND receiver_id=$2 AND status='pending'",
    )
    .bind(request)
    .bind(user)
    .fetch_optional(&pool)
    .await
    .map_err(err)?
    .ok_or(AuthError::NotFound)?;
    let mut tx = pool.begin().await.map_err(err)?;
    lock_pair(&mut tx, user, sender).await?;
    let updated = sqlx::query(
        "UPDATE friend_requests SET status=$1,resolved_at=now()
         WHERE id=$2 AND receiver_id=$3 AND sender_id=$4 AND status='pending'
           AND NOT EXISTS (
             SELECT 1 FROM user_blocks WHERE
               (blocker_id=$3 AND blocked_id=$4)
               OR (blocker_id=$4 AND blocked_id=$3)
           ) RETURNING id",
    )
    .bind(if accept { "accepted" } else { "declined" })
    .bind(request)
    .bind(user)
    .bind(sender)
    .fetch_optional(&mut *tx)
    .await
    .map_err(err)?
    .ok_or(AuthError::NotFound)?;
    let _: Uuid = updated.try_get("id").map_err(err)?;
    if accept {
        sqlx::query(
            "INSERT INTO friendships(user_low,user_high)
             VALUES(LEAST($1::uuid,$2::uuid),GREATEST($1::uuid,$2::uuid))
             ON CONFLICT DO NOTHING",
        )
        .bind(user)
        .bind(sender)
        .execute(&mut *tx)
        .await
        .map_err(err)?;
    }
    tx.commit().await.map_err(err)?;
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn accept(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(request): Path<String>,
) -> Result<StatusCode, AuthError> {
    resolve(state, headers, request, true).await
}
pub(crate) async fn decline(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(request): Path<String>,
) -> Result<StatusCode, AuthError> {
    resolve(state, headers, request, false).await
}

pub(crate) async fn list_friends(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<UserLinkView>>, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let rows = sqlx::query(
        "SELECT u.id AS user_id,u.handle,u.display_name
         FROM friendships f JOIN users u ON
           u.id=CASE WHEN f.user_low=$1 THEN f.user_high ELSE f.user_low END
         WHERE f.user_low=$1 OR f.user_high=$1
         ORDER BY u.handle LIMIT 200",
    )
    .bind(user)
    .fetch_all(&pool)
    .await
    .map_err(err)?;
    Ok(Json(rows.iter().map(user_view).collect::<Result<_, _>>()?))
}

pub(crate) async fn remove_friend(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(raw): Path<String>,
) -> Result<StatusCode, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let other = id(&raw)?;
    let mut tx = pool.begin().await.map_err(err)?;
    lock_pair(&mut tx, user, other).await?;
    let removed = sqlx::query(
        "DELETE FROM friendships WHERE user_low=LEAST($1::uuid,$2::uuid)
          AND user_high=GREATEST($1::uuid,$2::uuid)",
    )
    .bind(user)
    .bind(other)
    .execute(&mut *tx)
    .await
    .map_err(err)?;
    if removed.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    tx.commit().await.map_err(err)?;
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn block(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<BlockUser>,
) -> Result<StatusCode, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let other = id(&input.user_id)?;
    let mut tx = pool.begin().await.map_err(err)?;
    lock_pair(&mut tx, user, other).await?;
    sqlx::query(
        "INSERT INTO user_blocks(blocker_id,blocked_id) VALUES($1,$2)
         ON CONFLICT DO NOTHING",
    )
    .bind(user)
    .bind(other)
    .execute(&mut *tx)
    .await
    .map_err(err)?;
    sqlx::query(
        "DELETE FROM friendships WHERE user_low=LEAST($1::uuid,$2::uuid)
         AND user_high=GREATEST($1::uuid,$2::uuid)",
    )
    .bind(user)
    .bind(other)
    .execute(&mut *tx)
    .await
    .map_err(err)?;
    sqlx::query(
        "DELETE FROM friend_requests WHERE status='pending'
         AND LEAST(sender_id,receiver_id)=LEAST($1::uuid,$2::uuid)
         AND GREATEST(sender_id,receiver_id)=GREATEST($1::uuid,$2::uuid)",
    )
    .bind(user)
    .bind(other)
    .execute(&mut *tx)
    .await
    .map_err(err)?;
    tx.commit().await.map_err(err)?;
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn unblock(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(raw): Path<String>,
) -> Result<StatusCode, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let other = id(&raw)?;
    let mut tx = pool.begin().await.map_err(err)?;
    lock_pair(&mut tx, user, other).await?;
    let result = sqlx::query("DELETE FROM user_blocks WHERE blocker_id=$1 AND blocked_id=$2")
        .bind(user)
        .bind(other)
        .execute(&mut *tx)
        .await
        .map_err(err)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    tx.commit().await.map_err(err)?;
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn blocks(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<UserLinkView>>, AuthError> {
    let (pool, user) = actor(&state, &headers).await?;
    let rows = sqlx::query(
        "SELECT u.id AS user_id,u.handle,u.display_name
         FROM user_blocks b JOIN users u ON u.id=b.blocked_id
         WHERE b.blocker_id=$1 ORDER BY b.created_at DESC LIMIT 200",
    )
    .bind(user)
    .fetch_all(&pool)
    .await
    .map_err(err)?;
    Ok(Json(rows.iter().map(user_view).collect::<Result<_, _>>()?))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ids_reject_malformed_values() {
        assert!(id("garbage").is_err());
        assert!(id("00000000-0000-0000-0000-000000000001").is_ok());
    }
}
