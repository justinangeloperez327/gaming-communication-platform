//! Opaque-token authentication. Token plaintext is returned only in the
//! response that creates/rotates it; PostgreSQL persists SHA-256 digests.
//! No passwords, raw tokens, or credential-bearing DATABASE_URL values are logged.

use std::{
    sync::{Arc, OnceLock},
    time::Duration,
};

use argon2::{
    Algorithm, Argon2, Params, PasswordHash, PasswordHasher, PasswordVerifier, Version,
    password_hash::SaltString,
};
use axum::{
    Json,
    extract::State,
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
};
use rand::{RngCore, rngs::OsRng};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use sqlx::{PgPool, Row, types::Uuid};
use tokio::{sync::Semaphore, task, time::timeout};

use crate::AppState;

const ACCESS_SECONDS: i64 = 15 * 60;
const TOKEN_BYTES: usize = 32;
const MAX_PASSWORD_BYTES: usize = 128;

// Argon2 is deliberately memory-expensive. Limit simultaneous hash/verify
// operations to prevent unbounded spawn_blocking + memory pressure.
static PASSWORD_WORKERS: OnceLock<Arc<Semaphore>> = OnceLock::new();

fn workers() -> Arc<Semaphore> {
    PASSWORD_WORKERS
        .get_or_init(|| Arc::new(Semaphore::new(4)))
        .clone()
}

#[derive(Deserialize)]
pub struct RegisterRequest {
    handle: String,
    display_name: String,
    email: String,
    password: String,
}

#[derive(Deserialize)]
pub struct LoginRequest {
    email: String,
    password: String,
}

#[derive(Deserialize)]
pub struct RefreshRequest {
    refresh_token: String,
}

#[derive(Deserialize)]
pub struct ProfileRequest {
    display_name: String,
}

#[derive(Serialize)]
pub struct UserView {
    id: String,
    handle: String,
    display_name: String,
    email: String,
}

#[derive(Serialize)]
pub struct SessionResponse {
    user: UserView,
    access_token: String,
    refresh_token: String,
    token_type: &'static str,
    expires_in: i64,
}

#[derive(Serialize)]
struct ErrorBody {
    error: &'static str,
}

#[derive(Debug)]
pub enum AuthError {
    InvalidRequest,
    Unauthorized,
    Conflict,
    Busy,
    Unavailable,
}

impl IntoResponse for AuthError {
    fn into_response(self) -> Response {
        let (status, error) = match self {
            Self::InvalidRequest => (StatusCode::BAD_REQUEST, "invalid_request"),
            Self::Unauthorized => (StatusCode::UNAUTHORIZED, "unauthorized"),
            Self::Conflict => (StatusCode::CONFLICT, "conflict"),
            Self::Busy => (StatusCode::TOO_MANY_REQUESTS, "temporarily_busy"),
            Self::Unavailable => (StatusCode::SERVICE_UNAVAILABLE, "service_unavailable"),
        };
        (status, Json(ErrorBody { error })).into_response()
    }
}

fn database(state: &AppState) -> Result<&PgPool, AuthError> {
    state.database.as_ref().ok_or(AuthError::Unavailable)
}

fn valid_handle(handle: &str) -> bool {
    handle.len() >= 3
        && handle.len() <= 32
        && handle
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_')
}

fn valid_display_name(name: &str) -> bool {
    name == name.trim()
        && (1..=80).contains(&name.chars().count())
        && !name.chars().any(char::is_control)
}

fn canonical_email(input: &str) -> Result<String, AuthError> {
    if input.len() < 3
        || input.len() > 320
        || input != input.trim()
        || !input.is_ascii()
        || input.bytes().any(|b| b.is_ascii_whitespace())
    {
        return Err(AuthError::InvalidRequest);
    }
    let value = input.to_ascii_lowercase();
    let Some((local, domain)) = value.split_once('@') else {
        return Err(AuthError::InvalidRequest);
    };
    if local.is_empty()
        || domain.is_empty()
        || domain.contains('@')
        || !domain.contains('.')
        || domain.starts_with('.')
        || domain.ends_with('.')
    {
        return Err(AuthError::InvalidRequest);
    }
    Ok(value)
}

fn valid_password(p: &str) -> bool {
    (12..=MAX_PASSWORD_BYTES).contains(&p.len()) && !p.as_bytes().contains(&0)
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        result.push(DIGITS[usize::from(byte >> 4)] as char);
        result.push(DIGITS[usize::from(byte & 0x0f)] as char);
    }
    result
}

fn token_digest(value: &str) -> Result<String, AuthError> {
    if value.len() != TOKEN_BYTES * 2 || !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(AuthError::Unauthorized);
    }
    Ok(hex(&Sha256::digest(value.as_bytes())))
}

fn random_token() -> String {
    let mut bytes = [0u8; TOKEN_BYTES];
    OsRng.fill_bytes(&mut bytes);
    hex(&bytes)
}

async fn hash_password(password: String) -> Result<String, AuthError> {
    let permit = timeout(Duration::from_secs(2), workers().acquire_owned())
        .await
        .map_err(|_| AuthError::Busy)?
        .map_err(|_| AuthError::Unavailable)?;
    task::spawn_blocking(move || {
        let _permit = permit;
        let params = Params::new(65536, 3, 1, None).map_err(|_| ())?;
        let salt = SaltString::generate(&mut OsRng);
        Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
            .hash_password(password.as_bytes(), &salt)
            .map(|phc| phc.to_string())
            .map_err(|_| ())
    })
    .await
    .map_err(|_| AuthError::Unavailable)?
    .map_err(|_| AuthError::Unavailable)
}

async fn verify_password(password: String, stored: String) -> Result<bool, AuthError> {
    let permit = timeout(Duration::from_secs(2), workers().acquire_owned())
        .await
        .map_err(|_| AuthError::Busy)?
        .map_err(|_| AuthError::Unavailable)?;
    task::spawn_blocking(move || {
        let _permit = permit;
        let Ok(parsed) = PasswordHash::new(&stored) else {
            return false;
        };
        Argon2::default()
            .verify_password(password.as_bytes(), &parsed)
            .is_ok()
    })
    .await
    .map_err(|_| AuthError::Unavailable)
}

fn read_user(row: &sqlx::postgres::PgRow) -> Result<UserView, AuthError> {
    Ok(UserView {
        id: row
            .try_get::<Uuid, _>("id")
            .map_err(|_| AuthError::Unavailable)?
            .to_string(),
        handle: row.try_get("handle").map_err(|_| AuthError::Unavailable)?,
        display_name: row
            .try_get("display_name")
            .map_err(|_| AuthError::Unavailable)?,
        email: row.try_get("email").map_err(|_| AuthError::Unavailable)?,
    })
}

async fn user_by_id(pool: &PgPool, id: Uuid) -> Result<UserView, AuthError> {
    let row = sqlx::query("SELECT id, handle, display_name, email FROM users WHERE id = $1")
        .bind(id)
        .fetch_optional(pool)
        .await
        .map_err(|_| AuthError::Unavailable)?
        .ok_or(AuthError::Unauthorized)?;
    read_user(&row)
}

fn session_response(user: UserView, access: String, refresh: String) -> SessionResponse {
    SessionResponse {
        user,
        access_token: access,
        refresh_token: refresh,
        token_type: "Bearer",
        expires_in: ACCESS_SECONDS,
    }
}

pub(crate) async fn register(
    State(state): State<AppState>,
    Json(input): Json<RegisterRequest>,
) -> Result<(StatusCode, Json<SessionResponse>), AuthError> {
    let pool = database(&state)?;
    if !valid_handle(&input.handle)
        || !valid_display_name(&input.display_name)
        || !valid_password(&input.password)
    {
        return Err(AuthError::InvalidRequest);
    }
    let email = canonical_email(&input.email)?;
    let hash = hash_password(input.password).await?;
    let access = random_token();
    let refresh = random_token();
    let mut tx = pool.begin().await.map_err(|_| AuthError::Unavailable)?;
    let inserted = sqlx::query(
        "INSERT INTO users (handle, display_name, email, password_hash)
         VALUES ($1, $2, $3, $4) RETURNING id, handle, display_name, email",
    )
    .bind(input.handle)
    .bind(input.display_name)
    .bind(email)
    .bind(hash)
    .fetch_one(&mut *tx)
    .await;
    let row = match inserted {
        Ok(row) => row,
        Err(sqlx::Error::Database(ref db)) if db.code().as_deref() == Some("23505") => {
            return Err(AuthError::Conflict);
        }
        Err(_) => return Err(AuthError::Unavailable),
    };
    let user = read_user(&row)?;
    let user_id: Uuid = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    sqlx::query(
        "INSERT INTO user_sessions
           (user_id, access_hash, refresh_hash, access_expires_at, refresh_expires_at)
         VALUES ($1, $2, $3, now() + interval '15 minutes',
                 now() + interval '30 days')",
    )
    .bind(user_id)
    .bind(token_digest(&access)?)
    .bind(token_digest(&refresh)?)
    .execute(&mut *tx)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    tx.commit().await.map_err(|_| AuthError::Unavailable)?;
    Ok((
        StatusCode::CREATED,
        Json(session_response(user, access, refresh)),
    ))
}

pub(crate) async fn login(
    State(state): State<AppState>,
    Json(input): Json<LoginRequest>,
) -> Result<Json<SessionResponse>, AuthError> {
    let pool = database(&state)?;
    let email = canonical_email(&input.email).map_err(|_| AuthError::Unauthorized)?;
    if input.password.len() > MAX_PASSWORD_BYTES || input.password.is_empty() {
        return Err(AuthError::Unauthorized);
    }
    let row = sqlx::query(
        "SELECT id, handle, display_name, email, password_hash
         FROM users WHERE lower(email) = $1",
    )
    .bind(email)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::Unauthorized)?;
    let stored: String = row
        .try_get("password_hash")
        .map_err(|_| AuthError::Unavailable)?;
    if !verify_password(input.password, stored).await? {
        return Err(AuthError::Unauthorized);
    }
    let user = read_user(&row)?;
    let id: Uuid = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    let access = random_token();
    let refresh = random_token();
    sqlx::query(
        "INSERT INTO user_sessions
           (user_id, access_hash, refresh_hash, access_expires_at, refresh_expires_at)
         VALUES ($1, $2, $3, now() + interval '15 minutes',
                 now() + interval '30 days')",
    )
    .bind(id)
    .bind(token_digest(&access)?)
    .bind(token_digest(&refresh)?)
    .execute(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    Ok(Json(session_response(user, access, refresh)))
}

fn bearer_digest(headers: &HeaderMap) -> Result<String, AuthError> {
    let raw = headers
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok())
        .ok_or(AuthError::Unauthorized)?;
    let token = raw.strip_prefix("Bearer ").ok_or(AuthError::Unauthorized)?;
    token_digest(token)
}

async fn authenticated_user(
    pool: &PgPool,
    headers: &HeaderMap,
) -> Result<(Uuid, UserView), AuthError> {
    let digest = bearer_digest(headers)?;
    let row = sqlx::query(
        "SELECT u.id, u.handle, u.display_name, u.email
         FROM user_sessions s JOIN users u ON u.id = s.user_id
         WHERE s.access_hash = $1 AND s.revoked_at IS NULL
           AND s.access_expires_at > now() AND s.refresh_expires_at > now()",
    )
    .bind(digest)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::Unauthorized)?;
    let id = row.try_get("id").map_err(|_| AuthError::Unavailable)?;
    Ok((id, read_user(&row)?))
}

pub(crate) async fn me(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<UserView>, AuthError> {
    let (_, user) = authenticated_user(database(&state)?, &headers).await?;
    Ok(Json(user))
}

pub(crate) async fn update_profile(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(input): Json<ProfileRequest>,
) -> Result<Json<UserView>, AuthError> {
    if !valid_display_name(&input.display_name) {
        return Err(AuthError::InvalidRequest);
    }
    let pool = database(&state)?;
    let (user_id, _) = authenticated_user(pool, &headers).await?;
    sqlx::query("UPDATE users SET display_name = $1, updated_at = now() WHERE id = $2")
        .bind(input.display_name)
        .bind(user_id)
        .execute(pool)
        .await
        .map_err(|_| AuthError::Unavailable)?;
    Ok(Json(user_by_id(pool, user_id).await?))
}

pub(crate) async fn refresh(
    State(state): State<AppState>,
    Json(input): Json<RefreshRequest>,
) -> Result<Json<SessionResponse>, AuthError> {
    let pool = database(&state)?;
    let old_digest = token_digest(&input.refresh_token)?;
    let access = random_token();
    let refresh = random_token();
    // Atomic compare-and-swap rotation ensures an old refresh token cannot
    // be used twice. The access token from the prior rotation is invalidated.
    let user_id: Uuid = sqlx::query_scalar(
        "UPDATE user_sessions SET access_hash = $1, refresh_hash = $2,
           access_expires_at = now() + interval '15 minutes',
           rotated_at = now()
         WHERE refresh_hash = $3 AND revoked_at IS NULL
           AND refresh_expires_at > now() + interval '15 minutes'
         RETURNING user_id",
    )
    .bind(token_digest(&access)?)
    .bind(token_digest(&refresh)?)
    .bind(old_digest)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?
    .ok_or(AuthError::Unauthorized)?;
    let user = user_by_id(pool, user_id).await?;
    Ok(Json(session_response(user, access, refresh)))
}

pub(crate) async fn logout(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<StatusCode, AuthError> {
    let pool = database(&state)?;
    let digest = bearer_digest(&headers)?;
    let result = sqlx::query(
        "UPDATE user_sessions SET revoked_at = now()
         WHERE access_hash = $1 AND revoked_at IS NULL
           AND access_expires_at > now()
         RETURNING id",
    )
    .bind(digest)
    .fetch_optional(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if result.is_none() {
        return Err(AuthError::Unauthorized);
    }
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn input_validation_is_bounded() {
        assert!(valid_handle("test_012"));
        assert!(!valid_handle("ADMIN"));
        assert!(!valid_handle("_"));
        assert!(valid_display_name("Player"));
        assert!(!valid_display_name(" Player "));
        assert!(!valid_display_name("\u{0000}"));
        assert!(valid_password("secure secret passphrase"));
        assert!(!valid_password("short"));
        assert_eq!(
            canonical_email("TEST@Example.COM").expect("email"),
            "test@example.com"
        );
        assert!(canonical_email("missing-at.example.com").is_err());
    }

    #[test]
    fn tokens_are_random_and_hashes_are_distinct() {
        let a = random_token();
        let b = random_token();
        assert_ne!(a, b);
        assert_eq!(a.len(), 64);
        assert_ne!(token_digest(&a).expect("digest"), a);
        assert_ne!(
            token_digest(&a).expect("digest"),
            token_digest(&b).expect("digest")
        );
        assert!(token_digest("short").is_err());
    }

    #[tokio::test]
    async fn argon2id_hash_verification() {
        let value = hash_password("long correct horse battery".into())
            .await
            .expect("argon2 hash");
        assert!(value.starts_with("$argon2id$"));
        assert!(
            verify_password("long correct horse battery".into(), value.clone())
                .await
                .expect("verification")
        );
        assert!(
            !verify_password("wrong password here".into(), value)
                .await
                .expect("wrong password")
        );
    }
}
