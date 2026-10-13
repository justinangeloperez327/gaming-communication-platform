//! Auth integration tests run against a disposable PostgreSQL service in CI.
//! No credentials, tokens or password hashes are written to test logs.
use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions};
use tower::ServiceExt;

async fn database() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(url) => url,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("GCP_TEST_DATABASE_URL must be valid Unicode"),
    };
    Some(
        PgPoolOptions::new()
            .max_connections(5)
            .connect(&url)
            .await
            .expect("disposable PostgreSQL test database"),
    )
}

async fn send(
    app: &Router,
    method: Method,
    path: &str,
    body: Option<Value>,
    token: Option<&str>,
) -> (StatusCode, Value) {
    let mut builder = Request::builder().uri(path).method(method);
    if body.is_some() {
        builder = builder.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(token) = token {
        builder = builder.header(header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let payload = body.map_or_else(Body::empty, |value| Body::from(value.to_string()));
    let response = app
        .clone()
        .oneshot(builder.body(payload).expect("valid request"))
        .await
        .expect("router response");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 16 * 1024)
        .await
        .expect("response body");
    let json = if bytes.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(&bytes).expect("JSON response")
    };
    (status, json)
}

#[tokio::test]
async fn registration_login_rotation_profile_and_revocation() {
    let Some(pool) = database().await else { return };
    MIGRATOR.run(&pool).await.expect("schema");
    let app = router_with_database(Some(pool.clone()));

    let suffix = format!("{:016x}", rand::random::<u64>());
    let handle = format!("u_{suffix}");
    let email = format!("test_{suffix}@example.test");
    let password = "correct horse battery stable 2026";

    let registration = json!({
        "handle": handle, "display_name": "First Player",
        "email": email, "password": password
    });
    let (status, account) = send(
        &app,
        Method::POST,
        "/v1/auth/register",
        Some(registration.clone()),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(account["user"]["handle"], handle);
    assert_eq!(account["user"]["email"], email);
    assert_eq!(account["token_type"], "Bearer");
    assert_eq!(account["expires_in"], 900);
    assert_eq!(account["access_token"].as_str().expect("access").len(), 64);
    assert_eq!(
        account["refresh_token"].as_str().expect("refresh").len(),
        64
    );
    assert!(account["user"].get("password_hash").is_none());

    // Case-insensitive uniqueness, without disclosing a stored password.
    let mut duplicate = registration;
    duplicate["handle"] = json!(format!("v_{suffix}"));
    duplicate["email"] = json!(email.to_ascii_uppercase());
    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/register",
        Some(duplicate),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CONFLICT);

    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/login",
        Some(json!({"email": email, "password": "wrong password"})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, login) = send(
        &app,
        Method::POST,
        "/v1/auth/login",
        Some(json!({"email": email.to_ascii_uppercase(), "password": password})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let access = login["access_token"]
        .as_str()
        .expect("login access")
        .to_owned();
    let refresh = login["refresh_token"]
        .as_str()
        .expect("login refresh")
        .to_owned();

    let (status, _) = send(&app, Method::GET, "/v1/me", None, None).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, me) = send(&app, Method::GET, "/v1/me", None, Some(&access)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(me["user_not_present"], Value::Null);
    assert_eq!(me["handle"], handle);

    let (status, profile) = send(
        &app,
        Method::PATCH,
        "/v1/me",
        Some(json!({"display_name":"Updated Player"})),
        Some(&access),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(profile["display_name"], "Updated Player");
    let (status, _) = send(
        &app,
        Method::PATCH,
        "/v1/me",
        Some(json!({"display_name":"  "})),
        Some(&access),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);

    let (status, rotated) = send(
        &app,
        Method::POST,
        "/v1/auth/refresh",
        Some(json!({"refresh_token": refresh})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let new_access = rotated["access_token"]
        .as_str()
        .expect("rotated access")
        .to_owned();
    let new_refresh = rotated["refresh_token"]
        .as_str()
        .expect("rotated refresh")
        .to_owned();
    assert_ne!(new_access, access);
    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/refresh",
        Some(json!({"refresh_token": refresh})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = send(&app, Method::GET, "/v1/me", None, Some(&access)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = send(&app, Method::GET, "/v1/me", None, Some(&new_access)).await;
    assert_eq!(status, StatusCode::OK);

    // The refresh token itself never appears in the database in plaintext.
    let digest_count: i64 =
        sqlx::query_scalar("SELECT count(*) FROM user_sessions WHERE refresh_hash = $1")
            .bind(&new_refresh)
            .fetch_one(&pool)
            .await
            .expect("plaintext token lookup");
    assert_eq!(digest_count, 0);

    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/logout",
        None,
        Some(&new_access),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = send(&app, Method::GET, "/v1/me", None, Some(&new_access)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/refresh",
        Some(json!({"refresh_token": new_refresh})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);

    // Other independent sessions need explicit logout; delete test user
    // and cascade its sessions to avoid leaking test fixture data.
    sqlx::query("DELETE FROM users WHERE handle = $1")
        .bind(handle)
        .execute(&pool)
        .await
        .expect("remove test account and sessions");
}

#[tokio::test]
async fn routes_fail_closed_without_postgres() {
    let app = router_with_database(None);
    let (status, _) = send(
        &app,
        Method::POST,
        "/v1/auth/login",
        Some(json!({"email":"x@y.test","password":"anything"})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    let (status, _) = send(&app, Method::GET, "/v1/me", None, None).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
}
