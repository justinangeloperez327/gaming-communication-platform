//! Community and channel operations with authorization at the SQL mutation boundary.
//! Nonmembers receive 404 for resource lookups. No message data is exposed.

use axum::{
    Json,
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, Row, types::Uuid};

use crate::{
    AppState,
    auth::{self, AuthError},
};

fn uuid(raw: &str) -> Result<Uuid, AuthError> {
    Uuid::parse_str(raw).map_err(|_| AuthError::InvalidRequest)
}

fn name(value: &str, limit: usize) -> bool {
    !value.is_empty()
        && value.trim() == value
        && value.chars().count() <= limit
        && !value.chars().any(char::is_control)
}

fn channel_name(value: &str) -> bool {
    (2..=80).contains(&value.len())
        && value.bytes().all(|byte| {
            byte.is_ascii_lowercase()
                || byte.is_ascii_digit()
                || byte == b'-'
                || byte == b'_'
        })
}

fn is_unique(error: &sqlx::Error) -> bool {
    matches!(error, sqlx::Error::Database(db) if db.code().as_deref() == Some("23505"))
}

fn db_error(error: sqlx::Error) -> AuthError {
    if is_unique(&error) {
        AuthError::Conflict
    } else {
        AuthError::Unavailable
    }
}

async fn actor(state: &AppState, headers: &HeaderMap) -> Result<(PgPool, Uuid), AuthError> {
    let pool = auth::database(state)?;
    let (id, _) = auth::authenticated_user(pool, headers).await?;
    Ok((pool.clone(), id))
}

async fn role(pool: &PgPool, user_id: Uuid, community: Uuid) -> Result<String, AuthError> {
    sqlx::query_scalar::<_, String>(
        "SELECT role FROM community_memberships
         WHERE user_id = $1 AND community_id = $2",
    )
    .bind(user_id)
    .bind(community)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::NotFound)
}

fn manager(role: &str) -> Result<(), AuthError> {
    if matches!(role, "owner" | "admin") {
        Ok(())
    } else {
        Err(AuthError::Forbidden)
    }
}

#[derive(Deserialize)]
pub(crate) struct NameInput {
    name: String,
}

#[derive(Deserialize)]
pub(crate) struct ChannelInput {
    name: String,
    channel_kind: String,
    position: i32,
}

#[derive(Deserialize)]
pub(crate) struct ChannelUpdate {
    name: String,
    position: i32,
}

#[derive(Deserialize)]
pub(crate) struct RoleInput {
    role: String,
}

#[derive(Deserialize)]
pub(crate) struct AcceptInvitation {
    token: String,
}

#[derive(Serialize)]
pub(crate) struct CommunityView {
    id: String,
    name: String,
    role: String,
}

#[derive(Serialize)]
pub(crate) struct MemberView {
    user_id: String,
    handle: String,
    display_name: String,
    role: String,
}

#[derive(Serialize)]
pub(crate) struct ChannelView {
    id: String,
    name: String,
    channel_kind: String,
    position: i32,
}

#[derive(Serialize)]
pub(crate) struct InvitationView {
    token: String,
    expires_in: u32,
}

fn community_row(row: &sqlx::postgres::PgRow) -> Result<CommunityView, AuthError> {
    let id: Uuid = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    Ok(CommunityView {
        id: id.to_string(),
        name: row.try_get("name").map_err(|_| AuthError::Unavailable)?,
        role: row.try_get("role").map_err(|_| AuthError::Unavailable)?,
    })
}

fn channel_row(row: &sqlx::postgres::PgRow) -> Result<ChannelView, AuthError> {
    let id: Uuid = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    Ok(ChannelView {
        id: id.to_string(),
        name: row.try_get("name").map_err(|_| AuthError::Unavailable)?,
        channel_kind: row.try_get("channel_kind").map_err(|_| AuthError::Unavailable)?,
        position: row.try_get("position").map_err(|_| AuthError::Unavailable)?,
    })
}

pub(crate) async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<NameInput>,
) -> Result<(StatusCode, Json<CommunityView>), AuthError> {
    if !name(&input.name, 100) {
        return Err(AuthError::InvalidRequest);
    }
    let (pool, user_id) = actor(&state, &headers).await?;
    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    let row = sqlx::query(
        "INSERT INTO communities (name, owner_id)
         VALUES ($1, $2) RETURNING id, name, 'owner'::text AS role",
    )
    .bind(input.name)
    .bind(user_id)
    .fetch_one(&mut *tx)
    .await
    .map_err(db_error)?;
    let view = community_row(&row)?;
    sqlx::query(
        "INSERT INTO community_memberships (community_id, user_id, role)
         VALUES ($1, $2, 'owner')",
    )
    .bind(uuid(&view.id)?)
    .bind(user_id)
    .execute(&mut *tx)
    .await
    .map_err(db_error)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok((StatusCode::CREATED, Json(view)))
}

pub(crate) async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<CommunityView>>, AuthError> {
    let (pool, user_id) = actor(&state, &headers).await?;
    let rows = sqlx::query(
        "SELECT c.id, c.name, m.role FROM communities c
         JOIN community_memberships m ON m.community_id = c.id
         WHERE m.user_id = $1 ORDER BY c.created_at DESC, c.id DESC LIMIT 100",
    )
    .bind(user_id)
    .fetch_all(&pool)
    .await
    .map_err(db_error)?;
    Ok(Json(rows.iter().map(community_row).collect::<Result<_, _>>()?))
}

pub(crate) async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
) -> Result<Json<CommunityView>, AuthError> {
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    let row = sqlx::query(
        "SELECT c.id, c.name, m.role FROM communities c
         JOIN community_memberships m ON m.community_id = c.id
         WHERE c.id = $1 AND m.user_id = $2",
    )
    .bind(community)
    .bind(user_id)
    .fetch_optional(&pool)
    .await
    .map_err(db_error)?
    .ok_or(AuthError::NotFound)?;
    Ok(Json(community_row(&row)?))
}

pub(crate) async fn rename(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
    Json(input): Json<NameInput>,
) -> Result<Json<CommunityView>, AuthError> {
    if !name(&input.name, 100) {
        return Err(AuthError::InvalidRequest);
    }
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let changed = sqlx::query(
        "UPDATE communities c SET name = $1, updated_at = now()
         WHERE c.id = $2 AND EXISTS (
           SELECT 1 FROM community_memberships m
           WHERE m.community_id = c.id AND m.user_id = $3
             AND m.role IN ('owner', 'admin')
         ) RETURNING id, name",
    )
    .bind(input.name)
    .bind(community)
    .bind(user_id)
    .fetch_optional(&pool)
    .await
    .map_err(db_error)?
    .ok_or(AuthError::NotFound)?;
    let id: Uuid = changed.try_get("id").map_err(|_| AuthError::Unavailable)?;
    Ok(Json(CommunityView {
        id: id.to_string(),
        name: changed.try_get("name").map_err(|_| AuthError::Unavailable)?,
        role: role(&pool, user_id, community).await?,
    }))
}

pub(crate) async fn delete(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
) -> Result<StatusCode, AuthError> {
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    if role(&pool, user_id, community).await? != "owner" {
        return Err(AuthError::Forbidden);
    }
    let result = sqlx::query("DELETE FROM communities WHERE id = $1 AND owner_id = $2")
        .bind(community)
        .bind(user_id)
        .execute(&pool)
        .await
        .map_err(db_error)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn members(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
) -> Result<Json<Vec<MemberView>>, AuthError> {
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    role(&pool, user_id, community).await?;
    let rows = sqlx::query(
        "SELECT m.user_id, u.handle, u.display_name, m.role
         FROM community_memberships m JOIN users u ON u.id = m.user_id
         WHERE m.community_id = $1 AND EXISTS (
           SELECT 1 FROM community_memberships viewer
           WHERE viewer.community_id = $1 AND viewer.user_id = $2
         ) ORDER BY m.joined_at ASC, m.user_id ASC LIMIT 200",
    )
    .bind(community)
    .bind(user_id)
    .fetch_all(&pool)
    .await
    .map_err(db_error)?;
    let views = rows.iter().map(|row| {
        let user: Uuid = row.try_get("user_id").map_err(|_| AuthError::Unavailable)?;
        Ok(MemberView {
            user_id: user.to_string(),
            handle: row.try_get("handle").map_err(|_| AuthError::Unavailable)?,
            display_name: row.try_get("display_name").map_err(|_| AuthError::Unavailable)?,
            role: row.try_get("role").map_err(|_| AuthError::Unavailable)?,
        })
    }).collect::<Result<Vec<_>, AuthError>>()?;
    Ok(Json(views))
}

pub(crate) async fn change_role(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, member_id)): Path<(String, String)>,
    Json(input): Json<RoleInput>,
) -> Result<StatusCode, AuthError> {
    if !matches!(input.role.as_str(), "admin" | "member") {
        return Err(AuthError::InvalidRequest);
    }
    let community = uuid(&community_id)?;
    let target = uuid(&member_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    if role(&pool, user_id, community).await? != "owner" {
        return Err(AuthError::Forbidden);
    }
    let result = sqlx::query(
        "UPDATE community_memberships m SET role = $1
         FROM communities c
         WHERE c.id = $2 AND c.owner_id = $3
           AND m.community_id = c.id AND m.user_id = $4
           AND m.role <> 'owner' AND m.user_id <> c.owner_id",
    )
    .bind(input.role)
    .bind(community)
    .bind(user_id)
    .bind(target)
    .execute(&pool)
    .await
    .map_err(db_error)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn remove_member(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, member_id)): Path<(String, String)>,
) -> Result<StatusCode, AuthError> {
    let community = uuid(&community_id)?;
    let target = uuid(&member_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let result = sqlx::query(
        "DELETE FROM community_memberships target
         USING community_memberships actor, communities c
         WHERE c.id = $1 AND actor.community_id = c.id
           AND actor.user_id = $2 AND target.community_id = c.id
           AND target.user_id = $3 AND target.role <> 'owner'
           AND target.user_id <> c.owner_id
           AND (actor.role = 'owner'
                OR (actor.role = 'admin' AND target.role = 'member'))",
    )
    .bind(community)
    .bind(user_id)
    .bind(target)
    .execute(&pool)
    .await
    .map_err(db_error)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    Ok(StatusCode::NO_CONTENT)
}

pub(crate) async fn invite(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
) -> Result<(StatusCode, Json<InvitationView>), AuthError> {
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let token = auth::random_token();
    let result = sqlx::query(
        "INSERT INTO community_invitations
         (community_id, created_by, token_hash, expires_at)
         SELECT $1, $2, $3, now() + interval '24 hours'
         WHERE EXISTS (
           SELECT 1 FROM community_memberships m
           WHERE m.community_id = $1 AND m.user_id = $2
             AND m.role IN ('owner', 'admin')
         )",
    )
    .bind(community)
    .bind(user_id)
    .bind(auth::token_digest(&token)?)
    .execute(&pool)
    .await
    .map_err(db_error)?;
    if result.rows_affected() != 1 {
        return Err(AuthError::NotFound);
    }
    Ok((
        StatusCode::CREATED,
        Json(InvitationView { token, expires_in: 24 * 60 * 60 }),
    ))
}

pub(crate) async fn accept_invite(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<AcceptInvitation>,
) -> Result<(StatusCode, Json<CommunityView>), AuthError> {
    // Invalid/used/expired tokens all receive the same response.
    let digest = auth::token_digest(&input.token).map_err(|_| AuthError::NotFound)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    let community: Uuid = sqlx::query_scalar(
        "UPDATE community_invitations ci SET used_at = now(), used_by = $2
         WHERE ci.token_hash = $1 AND ci.used_at IS NULL
           AND ci.expires_at > now()
           AND EXISTS (
             SELECT 1 FROM community_memberships creator
             WHERE creator.community_id = ci.community_id
               AND creator.user_id = ci.created_by
               AND creator.role IN ('owner', 'admin')
           )
         RETURNING ci.community_id",
    )
    .bind(digest)
    .bind(user_id)
    .fetch_optional(&mut *tx)
    .await
    .map_err(db_error)?
    .ok_or(AuthError::NotFound)?;
    let inserted = sqlx::query(
        "INSERT INTO community_memberships (community_id, user_id, role)
         VALUES ($1, $2, 'member')",
    )
    .bind(community)
    .bind(user_id)
    .execute(&mut *tx)
    .await;
    if let Err(err) = inserted {
        return Err(db_error(err));
    }
    let row = sqlx::query(
        "SELECT id, name, 'member'::text AS role FROM communities WHERE id = $1",
    )
    .bind(community)
    .fetch_one(&mut *tx)
    .await
    .map_err(db_error)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok((StatusCode::CREATED, Json(community_row(&row)?)))
}

pub(crate) async fn channels(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
) -> Result<Json<Vec<ChannelView>>, AuthError> {
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    role(&pool, user_id, community).await?;
    let rows = sqlx::query(
        "SELECT ch.id, ch.name, ch.channel_kind, ch.position
         FROM channels ch
         WHERE ch.community_id = $1 AND EXISTS (
           SELECT 1 FROM community_memberships m
           WHERE m.community_id = ch.community_id AND m.user_id = $2
         ) ORDER BY ch.position, ch.id LIMIT 200",
    )
    .bind(community)
    .bind(user_id)
    .fetch_all(&pool)
    .await
    .map_err(db_error)?;
    Ok(Json(rows.iter().map(channel_row).collect::<Result<_, _>>()?))
}

pub(crate) async fn create_channel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<String>,
    Json(input): Json<ChannelInput>,
) -> Result<(StatusCode, Json<ChannelView>), AuthError> {
    if !channel_name(&input.name)
        || !matches!(input.channel_kind.as_str(), "text" | "voice")
        || input.position < 0
    {
        return Err(AuthError::InvalidRequest);
    }
    let community = uuid(&community_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let row = sqlx::query(
        "INSERT INTO channels (community_id, name, channel_kind, position)
         SELECT $1, $2, $3, $4
         WHERE EXISTS (
           SELECT 1 FROM community_memberships m
           WHERE m.community_id = $1 AND m.user_id = $5
             AND m.role IN ('owner', 'admin')
         ) RETURNING id, name, channel_kind, position",
    )
    .bind(community)
    .bind(input.name)
    .bind(input.channel_kind)
    .bind(input.position)
    .bind(user_id)
    .fetch_optional(&pool)
    .await
    .map_err(db_error)?
    .ok_or(AuthError::NotFound)?;
    Ok((StatusCode::CREATED, Json(channel_row(&row)?)))
}

pub(crate) async fn update_channel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
    Json(input): Json<ChannelUpdate>,
) -> Result<Json<ChannelView>, AuthError> {
    if !channel_name(&input.name) || input.position < 0 {
        return Err(AuthError::InvalidRequest);
    }
    let community = uuid(&community_id)?;
    let channel = uuid(&channel_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let row = sqlx::query(
        "UPDATE channels ch SET name = $1, position = $2, updated_at = now()
         WHERE ch.id = $3 AND ch.community_id = $4
           AND EXISTS (
             SELECT 1 FROM community_memberships m
             WHERE m.community_id = ch.community_id AND m.user_id = $5
               AND m.role IN ('owner', 'admin')
           ) RETURNING id, name, channel_kind, position",
    )
    .bind(input.name)
    .bind(input.position)
    .bind(channel)
    .bind(community)
    .bind(user_id)
    .fetch_optional(&pool)
    .await
    .map_err(db_error)?
    .ok_or(AuthError::NotFound)?;
    Ok(Json(channel_row(&row)?))
}

pub(crate) async fn delete_channel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
) -> Result<StatusCode, AuthError> {
    let community = uuid(&community_id)?;
    let channel = uuid(&channel_id)?;
    let (pool, user_id) = actor(&state, &headers).await?;
    manager(&role(&pool, user_id, community).await?)?;
    let result = sqlx::query(
        "DELETE FROM channels ch
         WHERE ch.id = $1 AND ch.community_id = $2
           AND EXISTS (
             SELECT 1 FROM community_memberships m
             WHERE m.community_id = ch.community_id AND m.user_id = $3
               AND m.role IN ('owner', 'admin')
           )",
    )
    .bind(channel)
    .bind(community)
    .bind(user_id)
    .execute(&pool)
    .await
    .map_err(db_error)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn community_and_channel_names_are_bounded() {
        assert!(name("Gaming Hub", 100));
        assert!(!name(" Gaming Hub", 100));
        assert!(!name("x\n", 100));
        assert!(channel_name("team-voice_1"));
        assert!(!channel_name("Voice Chat"));
        assert!(!channel_name("-"));
        assert!(manager("owner").is_ok());
        assert!(manager("admin").is_ok());
        assert!(manager("member").is_err());
    }
}
