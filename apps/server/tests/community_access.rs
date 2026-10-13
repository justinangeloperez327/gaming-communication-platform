//! End-to-end community authorization and one-use invitation checks.
//! Uses the disposable GCP_TEST_DATABASE_URL database provided by CI.
use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions};
use tower::ServiceExt;

async fn test_database() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(value) => value,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("invalid GCP_TEST_DATABASE_URL"),
    };
    Some(
        PgPoolOptions::new()
            .max_connections(8)
            .connect(&url)
            .await
            .expect("disposable PostgreSQL is available"),
    )
}

async fn request(
    app: &Router,
    method: Method,
    uri: &str,
    body: Option<Value>,
    token: Option<&str>,
) -> (StatusCode, Value) {
    let mut builder = Request::builder().method(method).uri(uri);
    if body.is_some() {
        builder = builder.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(token) = token {
        builder = builder.header(header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let result = app
        .clone()
        .oneshot(
            builder
                .body(body.map_or_else(Body::empty, |v| Body::from(v.to_string())))
                .expect("valid request"),
        )
        .await
        .expect("router");
    let status = result.status();
    let bytes = to_bytes(result.into_body(), 16_384).await.expect("body");
    let data = if bytes.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(&bytes).expect("valid JSON response")
    };
    (status, data)
}

async fn register(app: &Router, prefix: &str) -> (String, String) {
    let random = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{random}");
    let (status, response) = request(
        app,
        Method::POST,
        "/v1/auth/register",
        Some(json!({
            "handle": handle,
            "display_name": "CI Player",
            "email": format!("{handle}@example.test"),
            "password": "strong unique test passphrase 2026"
        })),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    (
        response["access_token"]
            .as_str()
            .expect("access token")
            .to_owned(),
        response["user"]["id"].as_str().expect("user id").to_owned(),
    )
}

#[tokio::test]
async fn owner_admin_member_and_invitation_permission_boundaries() {
    let Some(pool) = test_database().await else {
        return;
    };
    MIGRATOR.run(&pool).await.expect("run migrations");
    let app = router_with_database(Some(pool.clone()));
    let (owner_token, owner_id) = register(&app, "o").await;
    let (member_token, member_id) = register(&app, "m").await;
    let (outsider_token, outsider_id) = register(&app, "x").await;

    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/communities",
        Some(json!({"name": "Unowned"})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, community) = request(
        &app,
        Method::POST,
        "/v1/communities",
        Some(json!({"name":"Gaming Hub"})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(community["role"], "owner");
    let id = community["id"].as_str().expect("community ID");
    let path = format!("/v1/communities/{id}");

    let (status, _) = request(
        &app, Method::GET, &path, None, Some(&outsider_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, outsider_list) = request(
        &app, Method::GET, "/v1/communities", None, Some(&outsider_token)
    ).await;
    assert_eq!(status, StatusCode::OK);
    assert!(outsider_list.as_array().expect("array").is_empty());

    let channel_path = format!("{path}/channels");
    let (status, _) = request(
        &app, Method::GET, &channel_path, None, Some(&outsider_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, text) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "general", "channel_kind": "text", "position": 0})),
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::CREATED);
    let channel_id = text["id"].as_str().expect("channel ID");
    let channel_item = format!("{channel_path}/{channel_id}");
    let (status, _) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "general", "channel_kind": "voice", "position": 1})),
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &channel_item,
        Some(json!({"name": "secret", "position": 1})),
        Some(&outsider_token),
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let invitations_path = format!("{path}/invitations");
    let (status, _) = request(
        &app, Method::POST, &invitations_path, None, Some(&outsider_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, invitation) = request(
        &app, Method::POST, &invitations_path, None, Some(&owner_token)
    ).await;
    assert_eq!(status, StatusCode::CREATED);
    let invite_token = invitation["token"].as_str().expect("invitation token");
    assert_eq!(invite_token.len(), 64);
    assert_eq!(invitation["expires_in"], 86_400);

    let (status, member_join) = request(
        &app,
        Method::POST,
        "/v1/invitations/accept",
        Some(json!({"token": invite_token})),
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(member_join["role"], "member");
    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/invitations/accept",
        Some(json!({"token": invite_token})),
        Some(&outsider_token),
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "member-cannot", "channel_kind": "voice", "position": 0})),
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, visible) = request(
        &app, Method::GET, &channel_path, None, Some(&member_token)
    ).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(visible.as_array().expect("channels").len(), 1);
    let (status, _) = request(
        &app,
        Method::POST,
        &invitations_path,
        None,
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let members_path = format!("{path}/members");
    let (status, _) = request(
        &app, Method::GET, &members_path, None, Some(&outsider_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, members) = request(
        &app, Method::GET, &members_path, None, Some(&member_token)
    ).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(members.as_array().expect("members").len(), 2);

    let role_path = format!("{members_path}/{member_id}");
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "owner"})),
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "admin"})),
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "admin"})),
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::NO_CONTENT);

    let (status, voice) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "team-voice", "channel_kind": "voice", "position": 1})),
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(voice["channel_kind"], "voice");

    let owner_role_path = format!("{members_path}/{owner_id}");
    let (status, _) = request(
        &app,
        Method::DELETE,
        &owner_role_path,
        None,
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &owner_role_path,
        Some(json!({"role": "member"})),
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(
        &app,
        Method::PATCH,
        &path,
        Some(json!({"name":"Renamed Community"})),
        Some(&member_token),
    ).await;
    assert_eq!(status, StatusCode::OK);

    let (status, _) = request(
        &app, Method::DELETE, &path, None, Some(&member_token)
    ).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = request(
        &app,
        Method::DELETE,
        &role_path,
        None,
        Some(&owner_token),
    ).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = request(
        &app, Method::GET, &channel_path, None, Some(&member_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(
        &app, Method::DELETE, &path, None, Some(&owner_token)
    ).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = request(
        &app, Method::GET, &path, None, Some(&owner_token)
    ).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    // Remove isolated fixtures. Community FK cascade already deleted channels,
    // memberships and invitation rows.
    for user in [owner_id, member_id, outsider_id] {
        sqlx::query("DELETE FROM users WHERE id = $1")
            .bind(sqlx::types::Uuid::parse_str(&user).expect("UUID"))
            .execute(&pool)
            .await
            .expect("cleanup user");
    }
}

#[tokio::test]
async fn community_routes_fail_closed_without_database() {
    let app = router_with_database(None);
    let (status, _) = request(
        &app, Method::GET, "/v1/communities", None, None
    ).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
}
