//! Local owner bootstrap and owner-only, email-bound account invitations.
//! No HTTP endpoint can claim ownership. Codes are stored only as SHA-256 hashes.

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

#[derive(Deserialize)]
pub struct OwnerInput {
    handle: String,
    display_name: String,
    email: String,
    password: String,
}

pub(crate) async fn is_owner(pool: &PgPool, user: Uuid) -> Result<bool, AuthError> {
    sqlx::query_scalar("SELECT EXISTS (SELECT 1 FROM server_ownership WHERE owner_id = $1)")
        .bind(user)
        .fetch_one(pool)
        .await
        .map_err(|_| AuthError::Unavailable)
}

/// Operator-only command, never mounted as an HTTP route. Existing accounts
/// require their current password and matching handle; profile data is preserved.
pub async fn bootstrap_owner(pool: &PgPool, input: OwnerInput) -> Result<Uuid, AuthError> {
    if !auth::valid_handle(&input.handle)
        || !auth::valid_display_name(&input.display_name)
        || !auth::valid_password(&input.password)
    {
        return Err(AuthError::InvalidRequest);
    }
    let email = auth::canonical_email(&input.email)?;
    let existing =
        sqlx::query("SELECT id, handle, password_hash FROM users WHERE lower(email) = $1")
            .bind(&email)
            .fetch_optional(pool)
            .await
            .map_err(|_| AuthError::Unavailable)?;
    let (existing_id, hash) = if let Some(row) = existing {
        let handle: String = row.try_get("handle").map_err(|_| AuthError::Unavailable)?;
        let stored: String = row
            .try_get("password_hash")
            .map_err(|_| AuthError::Unavailable)?;
        if handle != input.handle || !auth::verify_password(input.password, stored).await? {
            return Err(AuthError::Unauthorized);
        }
        (
            Some(
                row.try_get::<Uuid, _>("id")
                    .map_err(|_| AuthError::Unavailable)?,
            ),
            None,
        )
    } else {
        (None, Some(auth::hash_password(input.password).await?))
    };

    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    let owner: Option<Uuid> =
        sqlx::query_scalar("SELECT owner_id FROM server_ownership WHERE singleton = 1 FOR UPDATE")
            .fetch_one(&mut *tx)
            .await
            .map_err(|_| AuthError::Unavailable)?;
    if owner.is_some() {
        return Err(AuthError::Conflict);
    }
    let user = match existing_id {
        Some(id) => id,
        None => sqlx::query_scalar::<_, Uuid>(
            "INSERT INTO users (handle, display_name, email, password_hash)
             VALUES ($1, $2, $3, $4) RETURNING id",
        )
        .bind(input.handle)
        .bind(input.display_name)
        .bind(email)
        .bind(hash)
        .fetch_one(&mut *tx)
        .await
        .map_err(|_| AuthError::Conflict)?,
    };
    sqlx::query("UPDATE server_ownership SET owner_id = $1 WHERE singleton = 1")
        .bind(user)
        .execute(&mut *tx)
        .await
        .map_err(|_| AuthError::Unavailable)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok(user)
}

#[derive(Deserialize)]
pub(crate) struct InvitationInput {
    email: String,
}

#[derive(Serialize)]
pub(crate) struct InvitationView {
    id: String,
    email: String,
    expires_in: i64,
}

#[derive(Serialize)]
pub(crate) struct CreatedInvitation {
    #[serde(flatten)]
    invitation: InvitationView,
    token: String,
}

async fn owner(state: &AppState, headers: &HeaderMap) -> Result<(PgPool, Uuid), AuthError> {
    let pool = auth::database(state)?;
    let (user, _) = auth::authenticated_user(pool, headers).await?;
    if !is_owner(pool, user).await? {
        return Err(AuthError::Forbidden);
    }
    Ok((pool.clone(), user))
}

pub(crate) async fn create_invitation(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<InvitationInput>,
) -> Result<(StatusCode, Json<CreatedInvitation>), AuthError> {
    let (pool, user) = owner(&state, &headers).await?;
    let email = auth::canonical_email(&input.email)?;
    let token = auth::random_token();
    let digest = auth::token_digest(&token)?;
    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    // Serialize the active-invitation bound and recheck ownership at mutation.
    let _: Uuid = sqlx::query_scalar(
        "SELECT owner_id FROM server_ownership WHERE singleton = 1 AND owner_id = $1 FOR UPDATE",
    )
    .bind(user)
    .fetch_optional(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::Forbidden)?;
    sqlx::query("DELETE FROM account_invitations WHERE expires_at < now() - interval '7 days'")
        .execute(&mut *tx)
        .await
        .map_err(|_| AuthError::Unavailable)?;
    let count: i64 = sqlx::query_scalar(
        "SELECT count(*) FROM account_invitations
         WHERE used_at IS NULL AND revoked_at IS NULL AND expires_at > now()",
    )
    .fetch_one(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if count >= 128 {
        return Err(AuthError::Busy);
    }
    let id: Uuid = sqlx::query_scalar(
        "INSERT INTO account_invitations (email, token_hash, created_by)
         VALUES ($1, $2, $3) RETURNING id",
    )
    .bind(&email)
    .bind(digest)
    .bind(user)
    .fetch_one(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok((
        StatusCode::CREATED,
        Json(CreatedInvitation {
            invitation: InvitationView {
                id: id.to_string(),
                email,
                expires_in: 24 * 60 * 60,
            },
            token,
        }),
    ))
}

pub(crate) async fn list_invitations(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<InvitationView>>, AuthError> {
    let (pool, _) = owner(&state, &headers).await?;
    let rows = sqlx::query(
        "SELECT id, email, greatest(0, extract(epoch FROM expires_at - now())::bigint) AS ttl
         FROM account_invitations WHERE used_at IS NULL AND revoked_at IS NULL AND expires_at > now()
         ORDER BY created_at DESC, id LIMIT 128",
    )
    .fetch_all(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    let mut invitations = Vec::with_capacity(rows.len());
    for row in rows {
        invitations.push(InvitationView {
            id: row
                .try_get::<Uuid, _>("id")
                .map_err(|_| AuthError::Unavailable)?
                .to_string(),
            email: row.try_get("email").map_err(|_| AuthError::Unavailable)?,
            expires_in: row.try_get("ttl").map_err(|_| AuthError::Unavailable)?,
        });
    }
    Ok(Json(invitations))
}

pub(crate) async fn revoke_invitation(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<StatusCode, AuthError> {
    let (pool, user) = owner(&state, &headers).await?;
    let id = Uuid::parse_str(&id).map_err(|_| AuthError::InvalidRequest)?;
    let result = sqlx::query(
        "UPDATE account_invitations SET revoked_at = clock_timestamp()
         WHERE id = $1 AND EXISTS (SELECT 1 FROM server_ownership WHERE owner_id = $2)",
    )
    .bind(id)
    .bind(user)
    .execute(&pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if result.rows_affected() == 0 {
        return Err(AuthError::NotFound);
    }
    Ok(StatusCode::NO_CONTENT)
}
