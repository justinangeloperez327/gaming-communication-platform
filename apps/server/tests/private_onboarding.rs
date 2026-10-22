//! Real PostgreSQL owner/bootstrap and invitation boundaries, in isolated schemas.
use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use gaming_comms_server::{
    database::MIGRATOR,
    onboarding::{OwnerInput, bootstrap_owner},
    router_with_database,
};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use tower::ServiceExt;

mod support;

async fn fixture() -> Option<(PgPool, PgPool, String)> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(value) => value,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("Invalid test database environment"),
    };
    let admin = PgPoolOptions::new()
        .max_connections(2)
        .connect(&url)
        .await
        .expect("test database");
    let schema = format!("onboarding_{}", Uuid::from_u128(rand::random()).simple());
    // Identifier is generated here, never taken from input or a connection URL.
    sqlx::query(&format!("CREATE SCHEMA {schema}"))
        .execute(&admin)
        .await
        .expect("test schema");
    let path = schema.clone();
    let pool = PgPoolOptions::new()
        .max_connections(6)
        .after_connect(move |connection, _| {
            let path = path.clone();
            Box::pin(async move {
                sqlx::query("SELECT set_config('search_path', $1, false)")
                    .bind(path)
                    .execute(connection)
                    .await?;
                Ok(())
            })
        })
        .connect(&url)
        .await
        .expect("isolated pool");
    MIGRATOR
        .run(&pool)
        .await
        .expect("isolated schema migrations");
    Some((admin, pool, schema))
}

async fn request(
    app: &Router,
    method: Method,
    path: &str,
    body: Option<Value>,
    token: Option<&str>,
) -> (StatusCode, Value) {
    let mut request = Request::builder().uri(path).method(method);
    if body.is_some() {
        request = request.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(token) = token {
        request = request.header(header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let response = app
        .clone()
        .oneshot(
            request
                .body(body.map_or_else(Body::empty, |v| Body::from(v.to_string())))
                .expect("request"),
        )
        .await
        .expect("response");
    assert_eq!(response.headers()[header::CACHE_CONTROL], "no-store");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 65536)
        .await
        .expect("bounded body");
    (
        status,
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(&bytes).expect("JSON")
        },
    )
}

fn account(handle: &str, email: &str) -> Value {
    json!({"handle":handle, "email":email, "display_name":"Original player", "password":"owner-test-passphrase-2026"})
}

fn owner_input(value: Value) -> OwnerInput {
    serde_json::from_value(value).expect("owner fixture")
}

async fn invite(app: &Router, owner: &str, email: &str) -> Value {
    let (status, invitation) = request(
        app,
        Method::POST,
        "/v1/account-invitations",
        Some(json!({"email":email})),
        Some(owner),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(invitation["expires_in"], 86400);
    invitation
}

async fn cleanup(admin: PgPool, pool: PgPool, schema: String) {
    pool.close().await;
    sqlx::query(&format!("DROP SCHEMA {schema} CASCADE"))
        .execute(&admin)
        .await
        .expect("remove isolated schema");
    admin.close().await;
}

#[tokio::test]
async fn one_operator_owner_and_single_use_email_bound_invitations() {
    let Some((admin, pool, schema)) = fixture().await else {
        return;
    };
    let app = router_with_database(Some(pool.clone()));
    let first = account("first_owner", "owner1@example.test");
    let second = account("second_owner", "owner2@example.test");
    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/auth/register",
        Some(first.clone()),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/auth/bootstrap-owner",
        Some(first.clone()),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (a, b) = tokio::join!(
        bootstrap_owner(&pool, owner_input(first.clone())),
        bootstrap_owner(&pool, owner_input(second.clone()))
    );
    assert_ne!(a.is_ok(), b.is_ok());
    let winning = if a.is_ok() { first } else { second };
    assert!(
        bootstrap_owner(
            &pool,
            owner_input(account("other_owner", "owner3@example.test"))
        )
        .await
        .is_err()
    );
    let users: i64 = sqlx::query_scalar("SELECT count(*) FROM users")
        .fetch_one(&pool)
        .await
        .expect("users");
    assert_eq!(users, 1);
    let (status, login) = request(&app, Method::POST, "/v1/auth/login", Some(winning), None).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(login["server_owner"], true);
    let owner = login["access_token"].as_str().expect("owner token");
    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/account-invitations",
        Some(json!({"email":"friend@example.test"})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);

    let invitation = invite(&app, owner, "FRIEND@Example.Test").await;
    let mut friend = account("friend", "friend@example.test");
    friend["invitation_token"] = invitation["token"].clone();
    friend["server_owner"] = json!(true); // Client input cannot assign privileges.
    let mut wrong_email = friend.clone();
    wrong_email["email"] = json!("other@example.test");
    let (status, error) = request(
        &app,
        Method::POST,
        "/v1/auth/register",
        Some(wrong_email),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert_eq!(error["error"], "invitation_invalid");
    let (status, registered) = request(
        &app,
        Method::POST,
        "/v1/auth/register",
        Some(friend.clone()),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(registered["server_owner"], false);
    let member = registered["access_token"].as_str().expect("member token");
    let (status, _) = request(&app, Method::POST, "/v1/auth/register", Some(friend), None).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    for (method, path, body) in [
        (Method::GET, "/v1/account-invitations".to_owned(), None),
        (
            Method::POST,
            "/v1/account-invitations".to_owned(),
            Some(json!({"email":"x@example.test"})),
        ),
        (
            Method::DELETE,
            format!(
                "/v1/account-invitations/{}",
                invitation["id"].as_str().expect("id")
            ),
            None,
        ),
    ] {
        assert_eq!(
            request(&app, method, &path, body, Some(member)).await.0,
            StatusCode::FORBIDDEN
        );
    }
    let stored: i64 =
        sqlx::query_scalar("SELECT count(*) FROM account_invitations WHERE token_hash = $1")
            .bind(invitation["token"].as_str().expect("token"))
            .fetch_one(&pool)
            .await
            .expect("plaintext lookup");
    assert_eq!(stored, 0);

    // Failed account insertion must roll back consumption of a valid invite.
    let duplicate = invite(&app, owner, "friend@example.test").await;
    let mut collision = account("another_handle", "friend@example.test");
    collision["invitation_token"] = duplicate["token"].clone();
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/auth/register",
            Some(collision),
            None
        )
        .await
        .0,
        StatusCode::CONFLICT
    );
    let unused: bool =
        sqlx::query_scalar("SELECT used_at IS NULL FROM account_invitations WHERE id = $1")
            .bind(
                duplicate["id"]
                    .as_str()
                    .expect("id")
                    .parse::<Uuid>()
                    .expect("UUID"),
            )
            .fetch_one(&pool)
            .await
            .expect("invite state");
    assert!(unused);

    let raced = invite(&app, owner, "race@example.test").await;
    let mut racer = account("racer_a", "race@example.test");
    racer["invitation_token"] = raced["token"].clone();
    let mut rival = racer.clone();
    rival["handle"] = json!("racer_b");
    let (a, b) = tokio::join!(
        request(&app, Method::POST, "/v1/auth/register", Some(racer), None),
        request(&app, Method::POST, "/v1/auth/register", Some(rival), None)
    );
    assert!(
        (a.0 == StatusCode::CREATED && b.0 == StatusCode::FORBIDDEN)
            || (b.0 == StatusCode::CREATED && a.0 == StatusCode::FORBIDDEN)
    );

    for (handle, expired) in [("revoked", false), ("expired", true)] {
        let email = format!("{handle}@example.test");
        let code = invite(&app, owner, &email).await;
        let id = code["id"].as_str().expect("id");
        if expired {
            sqlx::query("UPDATE account_invitations SET created_at = now() - interval '48 hours', expires_at = now() - interval '24 hours' WHERE id = $1")
                .bind(id.parse::<Uuid>().expect("UUID")).execute(&pool).await.expect("expire fixture");
        } else {
            assert_eq!(
                request(
                    &app,
                    Method::DELETE,
                    &format!("/v1/account-invitations/{id}"),
                    None,
                    Some(owner)
                )
                .await
                .0,
                StatusCode::NO_CONTENT
            );
        }
        let mut signup = account(handle, &email);
        signup["invitation_token"] = code["token"].clone();
        assert_eq!(
            request(&app, Method::POST, "/v1/auth/register", Some(signup), None)
                .await
                .0,
            StatusCode::FORBIDDEN
        );
    }
    let (status, pending) = request(
        &app,
        Method::GET,
        "/v1/account-invitations",
        None,
        Some(owner),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        pending
            .as_array()
            .expect("pending")
            .iter()
            .all(|item| item.get("token").is_none())
    );
    let (status, rotated) = request(
        &app,
        Method::POST,
        "/v1/auth/refresh",
        Some(json!({"refresh_token":login["refresh_token"]})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(rotated["server_owner"], true);
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/account-invitations",
            Some(json!({"email":"x@example.test"})),
            Some(owner)
        )
        .await
        .0,
        StatusCode::UNAUTHORIZED
    );
    cleanup(admin, pool, schema).await;
}

#[tokio::test]
async fn existing_account_claim_requires_password_and_preserves_profile() {
    let Some((admin, pool, schema)) = fixture().await else {
        return;
    };
    let app = router_with_database(Some(pool.clone()));
    let original = account("existing_player", "existing@example.test");
    let mut registration = original.clone();
    registration["invitation_token"] =
        json!(support::invitation(&pool, "existing@example.test").await);
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/auth/register",
            Some(registration),
            None
        )
        .await
        .0,
        StatusCode::CREATED
    );
    let mut wrong = original.clone();
    wrong["password"] = json!("wrong-password-1234");
    assert!(bootstrap_owner(&pool, owner_input(wrong)).await.is_err());
    let mut claim = original.clone();
    claim["display_name"] = json!("Do not overwrite");
    bootstrap_owner(&pool, owner_input(claim))
        .await
        .expect("operator claims existing account");
    let (status, login) = request(&app, Method::POST, "/v1/auth/login", Some(original), None).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(login["server_owner"], true);
    assert_eq!(login["user"]["display_name"], "Original player");
    cleanup(admin, pool, schema).await;
}
