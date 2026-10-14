//! PostgreSQL-backed friend/DM privacy integration. All fixtures are isolated
//! by random account IDs. No tokens or DM content are printed to test logs.
use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use tower::ServiceExt;

async fn pool() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(v) => v,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("test URL invalid"),
    };
    Some(
        PgPoolOptions::new()
            .max_connections(8)
            .connect(&url)
            .await
            .expect("CI PostgreSQL"),
    )
}

async fn call(
    app: &Router,
    method: Method,
    path: &str,
    body: Option<Value>,
    token: Option<&str>,
) -> (StatusCode, Value) {
    let mut request = Request::builder().method(method).uri(path);
    if body.is_some() {
        request = request.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(value) = token {
        request = request.header(header::AUTHORIZATION, format!("Bearer {value}"));
    }
    let response = app
        .clone()
        .oneshot(
            request
                .body(body.map_or_else(Body::empty, |v| Body::from(v.to_string())))
                .expect("valid request"),
        )
        .await
        .expect("router");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 65536).await.expect("body");
    (
        status,
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(&bytes).expect("JSON response")
        },
    )
}

async fn create_user(app: &Router, prefix: &str) -> (String, String, String) {
    let random = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{random}");
    let (status, json) = call(
        app,
        Method::POST,
        "/v1/auth/register",
        Some(json!({
            "handle":handle,"email":format!("{handle}@example.test"),
            "display_name":"DM Test Player","password":"long random testing password 2026"
        })),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    (
        json["access_token"].as_str().expect("token").to_owned(),
        json["user"]["id"].as_str().expect("ID").to_owned(),
        handle,
    )
}

#[tokio::test]
async fn friendships_blocking_and_private_messages() {
    let Some(db) = pool().await else { return };
    MIGRATOR.run(&db).await.expect("migrations");
    let app = router_with_database(Some(db.clone()));
    let (a, aid, ah) = create_user(&app, "fma").await;
    let (b, bid, bh) = create_user(&app, "fmb").await;
    let (c, cid, _) = create_user(&app, "fmc").await;

    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":bh})),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":bid})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND, "nonfriends cannot open DM");
    let (status, request) = call(
        &app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":bh})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let rid = request["id"].as_str().expect("request id");
    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":ah})),
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, incoming) = call(&app, Method::GET, "/v1/friends/requests", None, Some(&b)).await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        incoming
            .as_array()
            .expect("requests")
            .iter()
            .any(|v| v["id"] == rid)
    );
    let (status, _) = call(
        &app,
        Method::POST,
        &format!("/v1/friends/requests/{rid}/accept"),
        None,
        Some(&c),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = call(
        &app,
        Method::POST,
        &format!("/v1/friends/requests/{rid}/accept"),
        None,
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = call(
        &app,
        Method::POST,
        &format!("/v1/friends/requests/{rid}/accept"),
        None,
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, friends) = call(&app, Method::GET, "/v1/friends", None, Some(&a)).await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        friends
            .as_array()
            .expect("friends")
            .iter()
            .any(|v| v["user_id"] == bid)
    );

    let (status, opened) = call(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":bid})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let conversation = opened["id"].as_str().expect("conversation").to_string();
    let (status, reopened) = call(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":aid})),
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(reopened["id"], conversation);
    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":aid})),
        Some(&c),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let base = format!("/v1/dms/{conversation}/messages");
    let (status, _) = call(&app, Method::GET, &base, None, Some(&c)).await;
    assert_eq!(status, StatusCode::NOT_FOUND);

    let (status, first) = call(
        &app,
        Method::POST,
        &base,
        Some(json!({"body":"Private DM content"})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(first["author_id"], aid);
    let mid = first["id"].as_str().expect("message");
    let (status, _) = call(
        &app,
        Method::PATCH,
        &format!("{base}/{mid}"),
        Some(json!({"body":"unauthorized edit"})),
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, edited) = call(
        &app,
        Method::PATCH,
        &format!("{base}/{mid}"),
        Some(json!({"body":"edited privately"})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(edited["body"], "edited privately");
    let (status, _) = call(
        &app,
        Method::DELETE,
        &format!("{base}/{mid}"),
        None,
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    for n in 0..5 {
        let (status, _) = call(
            &app,
            Method::POST,
            &base,
            Some(json!({"body":format!("message {n}")})),
            Some(&b),
        )
        .await;
        assert_eq!(status, StatusCode::CREATED);
    }
    let (status, p1) = call(
        &app,
        Method::GET,
        &format!("{base}?limit=2"),
        None,
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(p1["messages"].as_array().expect("messages").len(), 2);
    let cursor = p1["next_before"].as_str().expect("next page");
    let (status, p2) = call(
        &app,
        Method::GET,
        &format!("{base}?limit=2&before={cursor}"),
        None,
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    assert_ne!(p1["messages"][0]["id"], p2["messages"][0]["id"]);
    let (status, _) = call(
        &app,
        Method::GET,
        &format!("{base}?limit=101"),
        None,
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = call(
        &app,
        Method::DELETE,
        &format!("{base}/{mid}"),
        None,
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, history) = call(
        &app,
        Method::GET,
        &format!("{base}?limit=20"),
        None,
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let tombstone = history["messages"]
        .as_array()
        .expect("messages")
        .iter()
        .find(|m| m["id"] == mid)
        .expect("tombstone");
    assert!(tombstone["body"].is_null());
    let persisted: String = sqlx::query_scalar("SELECT body FROM dm_messages WHERE id=$1")
        .bind(Uuid::parse_str(mid).expect("UUID"))
        .fetch_one(&db)
        .await
        .expect("DM body");
    assert_eq!(persisted, "[deleted]");

    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/blocks",
        Some(json!({"user_id":bid})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, friends) = call(&app, Method::GET, "/v1/friends", None, Some(&a)).await;
    assert_eq!(status, StatusCode::OK);
    assert!(friends.as_array().expect("array").is_empty());
    for token in [&a, &b] {
        let (status, _) = call(&app, Method::GET, &base, None, Some(token)).await;
        assert_eq!(
            status,
            StatusCode::NOT_FOUND,
            "blocking disables both directions"
        );
        let (status, _) = call(
            &app,
            Method::POST,
            &base,
            Some(json!({"body":"blocked DM"})),
            Some(token),
        )
        .await;
        assert_eq!(status, StatusCode::NOT_FOUND);
    }
    let (status, _) = call(
        &app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":ah})),
        Some(&b),
    )
    .await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, blocks) = call(&app, Method::GET, "/v1/blocks", None, Some(&a)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(blocks[0]["user_id"], bid);
    let (status, _) = call(
        &app,
        Method::DELETE,
        &format!("/v1/blocks/{bid}"),
        None,
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = call(&app, Method::GET, &base, None, Some(&b)).await;
    assert_eq!(status, StatusCode::NOT_FOUND, "unblock does not re-friend");

    // Explicit cleanup preserves other parallel CI fixtures.
    sqlx::query("DELETE FROM dm_conversations WHERE id=$1")
        .bind(Uuid::parse_str(&conversation).expect("UUID"))
        .execute(&db)
        .await
        .expect("delete conversation");
    for id in [aid, bid, cid] {
        sqlx::query("DELETE FROM users WHERE id=$1")
            .bind(Uuid::parse_str(&id).expect("UUID"))
            .execute(&db)
            .await
            .expect("delete fixture user");
    }
}

#[tokio::test]
async fn private_routes_fail_closed_without_database() {
    let app = router_with_database(None);
    let (status, _) = call(&app, Method::GET, "/v1/friends", None, None).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    let (status, _) = call(&app, Method::GET, "/v1/dms", None, None).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
}
