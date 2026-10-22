//! End-to-end community authorization and one-use invitation checks.
//! Uses the disposable GCP_TEST_DATABASE_URL database provided by CI.
mod support;

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

async fn register(app: &Router, pool: &PgPool, prefix: &str) -> (String, String) {
    let random = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{random}");
    let invitation = support::invitation(pool, &format!("{handle}@example.test")).await;
    let (status, response) = request(
        app,
        Method::POST,
        "/v1/auth/register",
        Some(json!({
            "handle": handle,
            "display_name": "CI Player",
            "email": format!("{handle}@example.test"),
            "invitation_token": invitation, "password": "strong unique test passphrase 2026"
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
    let (owner_token, owner_id) = register(&app, &pool, "o").await;
    let (member_token, member_id) = register(&app, &pool, "m").await;
    let (outsider_token, outsider_id) = register(&app, &pool, "x").await;

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

    let (status, _) = request(&app, Method::GET, &path, None, Some(&outsider_token)).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, outsider_list) = request(
        &app,
        Method::GET,
        "/v1/communities",
        None,
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert!(outsider_list.as_array().expect("array").is_empty());

    let channel_path = format!("{path}/channels");
    let (status, _) = request(
        &app,
        Method::GET,
        &channel_path,
        None,
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, text) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "general", "channel_kind": "text", "position": 0})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let channel_id = text["id"].as_str().expect("channel ID");
    let channel_item = format!("{channel_path}/{channel_id}");
    let (status, _) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "general", "channel_kind": "voice", "position": 1})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &channel_item,
        Some(json!({"name": "secret", "position": 1})),
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let invitations_path = format!("{path}/invitations");
    let (status, _) = request(
        &app,
        Method::POST,
        &invitations_path,
        None,
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, invitation) = request(
        &app,
        Method::POST,
        &invitations_path,
        None,
        Some(&owner_token),
    )
    .await;
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
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(member_join["role"], "member");
    let (status, _) = request(
        &app,
        Method::POST,
        "/v1/invitations/accept",
        Some(json!({"token": invite_token})),
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "member-cannot", "channel_kind": "voice", "position": 0})),
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, visible) =
        request(&app, Method::GET, &channel_path, None, Some(&member_token)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(visible.as_array().expect("channels").len(), 1);
    let (status, _) = request(
        &app,
        Method::POST,
        &invitations_path,
        None,
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let members_path = format!("{path}/members");
    let (status, _) = request(
        &app,
        Method::GET,
        &members_path,
        None,
        Some(&outsider_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, members) =
        request(&app, Method::GET, &members_path, None, Some(&member_token)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(members.as_array().expect("members").len(), 2);

    let role_path = format!("{members_path}/{member_id}");
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "owner"})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "admin"})),
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &role_path,
        Some(json!({"role": "admin"})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);

    let (status, voice) = request(
        &app,
        Method::POST,
        &channel_path,
        Some(json!({"name": "team-voice", "channel_kind": "voice", "position": 1})),
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(voice["channel_kind"], "voice");

    let owner_role_path = format!("{members_path}/{owner_id}");
    let (status, _) = request(
        &app,
        Method::DELETE,
        &owner_role_path,
        None,
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = request(
        &app,
        Method::PATCH,
        &owner_role_path,
        Some(json!({"role": "member"})),
        Some(&owner_token),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(
        &app,
        Method::PATCH,
        &path,
        Some(json!({"name":"Renamed Community"})),
        Some(&member_token),
    )
    .await;
    assert_eq!(status, StatusCode::OK);

    let (status, _) = request(&app, Method::DELETE, &path, None, Some(&member_token)).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _) = request(&app, Method::DELETE, &role_path, None, Some(&owner_token)).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = request(&app, Method::GET, &channel_path, None, Some(&member_token)).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, _) = request(&app, Method::DELETE, &path, None, Some(&owner_token)).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = request(&app, Method::GET, &path, None, Some(&owner_token)).await;
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
    let (status, _) = request(&app, Method::GET, "/v1/communities", None, None).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
}

async fn invitation(app: &Router, path: &str, token: &str) -> Value {
    let (status, value) = request(app, Method::POST, path, None, Some(token)).await;
    assert_eq!(status, StatusCode::CREATED);
    assert!(value["id"].as_str().is_some());
    value
}

#[tokio::test]
async fn pending_invitation_revocation_limits_and_self_leave() {
    let Some(pool) = test_database().await else {
        return;
    };
    MIGRATOR.run(&pool).await.expect("migrations");
    let app = router_with_database(Some(pool.clone()));
    let (owner, owner_id) = register(&app, &pool, "manage_o").await;
    let (member, member_id) = register(&app, &pool, "manage_m").await;
    let (other, other_id) = register(&app, &pool, "manage_x").await;
    let (status, community) = request(
        &app,
        Method::POST,
        "/v1/communities",
        Some(json!({"name":"Management acceptance"})),
        Some(&owner),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let id = community["id"].as_str().expect("community id");
    let path = format!("/v1/communities/{id}");
    let invites = format!("{path}/invitations");
    let membership = format!("{path}/membership");
    assert_eq!(
        request(&app, Method::DELETE, &membership, None, Some(&owner))
            .await
            .0,
        StatusCode::FORBIDDEN
    );
    assert_eq!(
        request(&app, Method::GET, &invites, None, Some(&other))
            .await
            .0,
        StatusCode::NOT_FOUND
    );
    let code = invitation(&app, &invites, &owner).await;
    let join = Some(json!({"token":code["token"]}));
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/invitations/accept",
            join,
            Some(&member)
        )
        .await
        .0,
        StatusCode::CREATED
    );
    let unused = invitation(&app, &invites, &owner).await;
    let revoke = format!("{invites}/{}", unused["id"].as_str().expect("invite id"));
    for (method, target) in [
        (Method::GET, invites.clone()),
        (Method::DELETE, revoke.clone()),
    ] {
        assert_eq!(
            request(&app, method, &target, None, Some(&member)).await.0,
            StatusCode::FORBIDDEN
        );
    }
    let (status, pending) = request(&app, Method::GET, &invites, None, Some(&owner)).await;
    assert_eq!(status, StatusCode::OK);
    let pending = pending.as_array().expect("pending list");
    assert_eq!(pending.len(), 1);
    assert_eq!(pending[0]["id"], unused["id"]);
    assert!(pending[0].get("token").is_none());
    assert!(pending[0].get("token_hash").is_none());
    // A valid manager of another community cannot revoke this community's code.
    let (status, second) = request(
        &app,
        Method::POST,
        "/v1/communities",
        Some(json!({"name":"Another community"})),
        Some(&other),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let wrong_scope = format!(
        "/v1/communities/{}/invitations/{}",
        second["id"].as_str().expect("id"),
        unused["id"].as_str().expect("id")
    );
    assert_eq!(
        request(&app, Method::DELETE, &wrong_scope, None, Some(&other))
            .await
            .0,
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        request(&app, Method::DELETE, &revoke, None, Some(&owner))
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/invitations/accept",
            Some(json!({"token":unused["token"]})),
            Some(&other)
        )
        .await
        .0,
        StatusCode::NOT_FOUND
    );
    // Revocation and redemption serialize: either membership commits or revocation wins.
    let raced = invitation(&app, &invites, &owner).await;
    let raced_path = format!("{invites}/{}", raced["id"].as_str().expect("id"));
    let (revoked, joined) = tokio::join!(
        request(&app, Method::DELETE, &raced_path, None, Some(&owner)),
        request(
            &app,
            Method::POST,
            "/v1/invitations/accept",
            Some(json!({"token":raced["token"]})),
            Some(&other)
        )
    );
    assert!(
        (revoked.0 == StatusCode::NO_CONTENT && joined.0 == StatusCode::NOT_FOUND)
            || (revoked.0 == StatusCode::NOT_FOUND && joined.0 == StatusCode::CREATED)
    );
    if joined.0 == StatusCode::CREATED {
        assert_eq!(
            request(&app, Method::DELETE, &membership, None, Some(&other))
                .await
                .0,
            StatusCode::NO_CONTENT
        );
    }
    // Promote and let the admin leave; old invitation codes from that creator stop working.
    let role_path = format!("{path}/members/{member_id}");
    assert_eq!(
        request(
            &app,
            Method::PATCH,
            &role_path,
            Some(json!({"role":"admin"})),
            Some(&owner)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    let orphaned = invitation(&app, &invites, &member).await;
    assert_eq!(
        request(&app, Method::DELETE, &membership, None, Some(&member))
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        request(&app, Method::GET, &path, None, Some(&member))
            .await
            .0,
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        request(
            &app,
            Method::POST,
            "/v1/invitations/accept",
            Some(json!({"token":orphaned["token"]})),
            Some(&other)
        )
        .await
        .0,
        StatusCode::NOT_FOUND
    );
    sqlx::query("DELETE FROM community_invitations WHERE community_id = $1")
        .bind(id.parse::<sqlx::types::Uuid>().expect("UUID"))
        .execute(&pool)
        .await
        .expect("clear local invitation fixtures");
    sqlx::query(
        "INSERT INTO community_invitations (community_id, created_by, token_hash, expires_at)
         SELECT $1::uuid, $2::uuid, replace($1::text, '-', '') || lpad(n::text, 32, '0'), now() + interval '1 hour'
         FROM generate_series(1, 199) AS n",
    )
    .bind(id.parse::<sqlx::types::Uuid>().expect("UUID"))
    .bind(owner_id.parse::<sqlx::types::Uuid>().expect("UUID"))
    .execute(&pool).await.expect("seed active invitation bound");
    let (a, b) = tokio::join!(
        request(&app, Method::POST, &invites, None, Some(&owner)),
        request(&app, Method::POST, &invites, None, Some(&owner))
    );
    assert!(
        (a.0 == StatusCode::CREATED && b.0 == StatusCode::TOO_MANY_REQUESTS)
            || (b.0 == StatusCode::CREATED && a.0 == StatusCode::TOO_MANY_REQUESTS)
    );
    let (status, pending) = request(&app, Method::GET, &invites, None, Some(&owner)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(pending.as_array().expect("bounded list").len(), 200);
    // Delete owned communities before users: owner_id intentionally restricts user deletion.
    assert_eq!(
        request(&app, Method::DELETE, &path, None, Some(&owner))
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    let second_path = format!("/v1/communities/{}", second["id"].as_str().expect("id"));
    assert_eq!(
        request(&app, Method::DELETE, &second_path, None, Some(&other))
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    for user in [owner_id, member_id, other_id] {
        sqlx::query("DELETE FROM users WHERE id = $1")
            .bind(user.parse::<sqlx::types::Uuid>().expect("UUID"))
            .execute(&pool)
            .await
            .expect("cleanup account");
    }
}
