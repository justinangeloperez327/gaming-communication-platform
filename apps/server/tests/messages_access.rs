//! PostgreSQL integration tests for text-channel message access and pagination.
//! Fixtures use distinct random accounts and clean up their communities/users.
use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use std::collections::HashSet;
use tower::ServiceExt;

async fn test_db() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(url) => url,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("GCP_TEST_DATABASE_URL must be valid Unicode"),
    };
    Some(PgPoolOptions::new().max_connections(6).connect(&url).await
        .expect("disposable PostgreSQL service must be available"))
}

async fn req(
    app: &Router,
    method: Method,
    url: &str,
    body: Option<Value>,
    token: Option<&str>,
) -> (StatusCode, Value) {
    let mut builder = Request::builder().method(method).uri(url);
    if body.is_some() {
        builder = builder.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(token) = token {
        builder = builder.header(header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let payload = body.map_or_else(Body::empty, |item| Body::from(item.to_string()));
    let response = app.clone().oneshot(builder.body(payload).expect("valid request")).await
        .expect("router response");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 65_536).await.expect("body");
    let json = if bytes.is_empty() { Value::Null } else {
        serde_json::from_slice(&bytes).expect("JSON response")
    };
    (status, json)
}

async fn register(app: &Router, prefix: &str) -> (String, String) {
    let id = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{id}");
    let (status, data) = req(app, Method::POST, "/v1/auth/register",
        Some(json!({
            "handle":handle, "display_name":"Player",
            "email":format!("{handle}@example.test"),
            "password":"long unique test password 2026"
        })), None).await;
    assert_eq!(status, StatusCode::CREATED);
    (
        data["access_token"].as_str().expect("access token").to_string(),
        data["user"]["id"].as_str().expect("user ID").to_string()
    )
}

async fn community(app: &Router, token: &str) -> String {
    let (status, value) = req(app, Method::POST, "/v1/communities",
        Some(json!({"name":"Text Room Test"})), Some(token)).await;
    assert_eq!(status, StatusCode::CREATED);
    value["id"].as_str().expect("community id").to_string()
}

async fn channel(app: &Router, community: &str, token: &str, kind: &str, name: &str) -> String {
    let url = format!("/v1/communities/{community}/channels");
    let (status, value) = req(app, Method::POST, &url,
        Some(json!({"name":name,"channel_kind":kind,"position":0})), Some(token)).await;
    assert_eq!(status, StatusCode::CREATED);
    value["id"].as_str().expect("channel id").to_string()
}

#[tokio::test]
async fn authorized_message_lifecycle_and_privacy() {
    let Some(pool) = test_db().await else { return };
    MIGRATOR.run(&pool).await.expect("migrations");
    let app = router_with_database(Some(pool.clone()));

    let (owner, owner_id) = register(&app, "own").await;
    let (member, member_id) = register(&app, "mem").await;
    let (outsider, outsider_id) = register(&app, "out").await;
    let community_a = community(&app, &owner).await;
    let community_b = community(&app, &outsider).await;
    let text_a = channel(&app, &community_a, &owner, "text", "general").await;
    let voice_a = channel(&app, &community_a, &owner, "voice", "voice").await;
    let text_b = channel(&app, &community_b, &outsider, "text", "general").await;
    let base = format!("/v1/communities/{community_a}/channels/{text_a}/messages");
    let voice = format!("/v1/communities/{community_a}/channels/{voice_a}/messages");
    let cross = format!("/v1/communities/{community_b}/channels/{text_a}/messages");
    let alien = format!("/v1/communities/{community_a}/channels/{text_b}/messages");

    let (code, _) = req(&app, Method::POST, &base, Some(json!({"body":"hello"})), None).await;
    assert_eq!(code, StatusCode::UNAUTHORIZED);
    let (code, _) = req(&app, Method::GET, &base, None, Some(&outsider)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    for url in [&cross, &alien, &voice] {
        let (code, _) = req(&app, Method::POST, url,
            Some(json!({"body":"cross resource"})), Some(&owner)).await;
        assert_eq!(code, StatusCode::NOT_FOUND);
    }
    let (code, _) = req(&app, Method::GET, &voice, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::POST, &base,
        Some(json!({"body":" \t\n"})), Some(&owner)).await;
    assert_eq!(code, StatusCode::BAD_REQUEST);
    let (code, _) = req(&app, Method::POST, &base,
        Some(json!({"body":"x".repeat(4001)})), Some(&owner)).await;
    assert_eq!(code, StatusCode::BAD_REQUEST);

    let invites = format!("/v1/communities/{community_a}/invitations");
    let (code, invite) = req(&app, Method::POST, &invites, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::CREATED);
    let (code, _) = req(&app, Method::POST, "/v1/invitations/accept",
        Some(json!({"token": invite["token"]})), Some(&member)).await;
    assert_eq!(code, StatusCode::CREATED);

    let (code, first) = req(&app, Method::POST, &base,
        Some(json!({"body":"Original text"})), Some(&member)).await;
    assert_eq!(code, StatusCode::CREATED);
    assert_eq!(first["body"], "Original text");
    assert_eq!(first["author_id"], member_id);
    assert!(first["created_at_ms"].as_i64().expect("timestamp") > 0);
    let id = first["id"].as_str().expect("message ID");
    let item = format!("{base}/{id}");

    let (code, value) = req(&app, Method::GET, &item, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::OK);
    assert_eq!(value["body"], "Original text");
    let (code, _) = req(&app, Method::GET, &item, None, Some(&outsider)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::GET, &format!("{alien}/{id}"), None, Some(&owner)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);

    let (code, _) = req(&app, Method::PATCH, &item,
        Some(json!({"body":"owner rewrote this"})), Some(&owner)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, updated) = req(&app, Method::PATCH, &item,
        Some(json!({"body":"Edited by author"})), Some(&member)).await;
    assert_eq!(code, StatusCode::OK);
    assert_eq!(updated["body"], "Edited by author");
    assert!(updated["edited_at_ms"].is_number());

    let (code, _) = req(&app, Method::DELETE, &item, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::NO_CONTENT);
    let (code, deleted) = req(&app, Method::GET, &item, None, Some(&member)).await;
    assert_eq!(code, StatusCode::OK);
    assert!(deleted["body"].is_null());
    assert!(deleted["deleted_at_ms"].is_number());
    let stored: String = sqlx::query_scalar("SELECT body FROM messages WHERE id = $1")
        .bind(Uuid::parse_str(id).expect("uuid"))
        .fetch_one(&pool).await.expect("stored tombstone");
    assert_eq!(stored, "[deleted]");
    let (code, _) = req(&app, Method::PATCH, &item,
        Some(json!({"body":"restore"})), Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::DELETE, &item, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);

    // Regular members can neither delete nor edit another author's content.
    let (code, second) = req(&app, Method::POST, &base,
        Some(json!({"body":"Owned by admin"})), Some(&owner)).await;
    assert_eq!(code, StatusCode::CREATED);
    let second_item = format!("{base}/{}", second["id"].as_str().expect("message"));
    let (code, _) = req(&app, Method::DELETE, &second_item, None, Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::PATCH, &second_item,
        Some(json!({"body":"tamper"})), Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);

    // Paging returns a stable descending keyset and no duplicates.
    for i in 0..5 {
        let (code, _) = req(&app, Method::POST, &base,
            Some(json!({"body":format!("page {i}")})), Some(&member)).await;
        assert_eq!(code, StatusCode::CREATED);
    }
    let mut cursor: Option<String> = None;
    let mut collected = HashSet::new();
    let mut pages = 0;
    loop {
        let url = cursor.as_ref().map_or_else(
            || format!("{base}?limit=2"),
            |current| format!("{base}?limit=2&before={current}"),
        );
        let (code, value) = req(&app, Method::GET, &url, None, Some(&member)).await;
        assert_eq!(code, StatusCode::OK);
        let page = value["messages"].as_array().expect("array");
        assert!(page.len() <= 2);
        for message in page {
            assert!(collected.insert(message["id"].as_str().expect("id").to_string()));
        }
        pages += 1;
        assert!(pages < 10, "pagination must terminate");
        cursor = value["next_before"].as_str().map(ToOwned::to_owned);
        if cursor.is_none() { break; }
    }
    assert_eq!(collected.len(), 7);
    for url in [format!("{base}?limit=0"), format!("{base}?limit=101"),
        format!("{base}?limit=1&before=invalid")] {
        let (code, _) = req(&app, Method::GET, &url, None, Some(&member)).await;
        assert_eq!(code, StatusCode::BAD_REQUEST);
    }

    // Membership is checked at every request, not just at login.
    let remove_member = format!("/v1/communities/{community_a}/members/{member_id}");
    let (code, _) = req(&app, Method::DELETE, &remove_member, None, Some(&owner)).await;
    assert_eq!(code, StatusCode::NO_CONTENT);
    let (code, _) = req(&app, Method::GET, &base, None, Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::POST, &base,
        Some(json!({"body":"after removal"})), Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);
    let (code, _) = req(&app, Method::DELETE, &second_item, None, Some(&member)).await;
    assert_eq!(code, StatusCode::NOT_FOUND);

    for (id, token) in [(&community_a, &owner), (&community_b, &outsider)] {
        let (code, _) = req(&app, Method::DELETE,
            &format!("/v1/communities/{id}"), None, Some(token)).await;
        assert_eq!(code, StatusCode::NO_CONTENT);
    }
    for id in [owner_id, member_id, outsider_id] {
        sqlx::query("DELETE FROM users WHERE id=$1")
            .bind(Uuid::parse_str(&id).expect("uuid"))
            .execute(&pool).await.expect("cleanup");
    }
}

#[tokio::test]
async fn message_routes_without_db_are_unavailable() {
    let app = router_with_database(None);
    let c = Uuid::nil();
    let uri = format!("/v1/communities/{c}/channels/{c}/messages");
    let (code, _) = req(&app, Method::GET, &uri, None, None).await;
    assert_eq!(code, StatusCode::SERVICE_UNAVAILABLE);
}
