//! End-to-end native WebSocket/HTTP integration with PostgreSQL.
use std::time::Duration;

mod support;

use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use futures_util::StreamExt;
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use tokio::{net::TcpListener, time::timeout};
use tokio_tungstenite::{
    connect_async,
    tungstenite::{Message as WsMessage, client::IntoClientRequest},
};
use tower::ServiceExt;

async fn db() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(url) => url,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("GCP_TEST_DATABASE_URL invalid"),
    };
    Some(
        PgPoolOptions::new()
            .max_connections(8)
            .connect(&url)
            .await
            .expect("CI PostgreSQL"),
    )
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
    let response = app
        .clone()
        .oneshot(
            builder
                .body(body.map_or_else(Body::empty, |v| Body::from(v.to_string())))
                .expect("request"),
        )
        .await
        .expect("router");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 16384).await.expect("body");
    let result = if bytes.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(&bytes).expect("JSON")
    };
    (status, result)
}

async fn signup(app: &Router, pool: &PgPool, prefix: &str) -> (String, String, String) {
    let suffix = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{suffix}");
    let invitation = support::invitation(pool, &format!("{handle}@example.test")).await;
    let (status, result) = req(
        app,
        Method::POST,
        "/v1/auth/register",
        Some(json!({
            "handle":handle,"display_name":"Socket Test",
            "email":format!("{handle}@example.test"),
            "invitation_token": invitation, "password":"this-is-a-long-test-password-2026"
        })),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    (
        result["access_token"].as_str().expect("token").to_owned(),
        result["user"]["id"].as_str().expect("id").to_owned(),
        handle,
    )
}

async fn next_kind<S>(socket: &mut S, kind: &str) -> Value
where
    S: futures_util::Stream<Item = Result<WsMessage, tokio_tungstenite::tungstenite::Error>>
        + Unpin,
{
    timeout(Duration::from_secs(12), async {
        loop {
            let message = socket.next().await.expect("open").expect("socket response");
            if let WsMessage::Text(s) = message {
                let v: Value = serde_json::from_str(&s).expect("JSON frame");
                if v["type"] == kind {
                    return v;
                }
            }
        }
    })
    .await
    .expect("expected event")
}

async fn befriend(app: &Router, a: &str, b: &str, handle: &str) {
    let (status, request) = req(
        app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":handle})),
        Some(a),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let rid = request["id"].as_str().expect("request");
    assert_eq!(
        req(
            app,
            Method::POST,
            &format!("/v1/friends/requests/{rid}/accept"),
            None,
            Some(b)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
}
type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;
async fn connect(url: &str, token: &str) -> Socket {
    let mut request = url.into_client_request().expect("request");
    request.headers_mut().insert(
        header::AUTHORIZATION,
        format!("Bearer {token}").parse().expect("header"),
    );
    connect_async(request).await.expect("authorized socket").0
}
async fn closes(socket: &mut Socket) {
    timeout(Duration::from_secs(3), async {
        loop {
            match socket.next().await {
                None | Some(Ok(WsMessage::Close(_))) | Some(Err(_)) => break,
                _ => {}
            }
        }
    })
    .await
    .expect("revocation wakes subscriber without waiting for periodic auth");
}
#[tokio::test]
async fn direct_events_retries_and_bilateral_revocation() {
    let Some(pool) = db().await else { return };
    MIGRATOR.run(&pool).await.expect("migrations");
    let app = router_with_database(Some(pool.clone()));
    let (a, aid, _) = signup(&app, &pool, "dma").await;
    let (b, bid, bh) = signup(&app, &pool, "dmb").await;
    let (c, cid, _) = signup(&app, &pool, "dmc").await;
    // Only the sender can cancel a still-pending outgoing request.
    let (status, pending) = req(
        &app,
        Method::POST,
        "/v1/friends/requests",
        Some(json!({"handle":bh})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let cancel = format!(
        "/v1/friends/requests/{}",
        pending["id"].as_str().expect("request")
    );
    for token in [&b, &c] {
        assert_eq!(
            req(&app, Method::DELETE, &cancel, None, Some(token))
                .await
                .0,
            StatusCode::NOT_FOUND
        );
    }
    let (_, incoming) = req(&app, Method::GET, "/v1/friends/requests", None, Some(&b)).await;
    assert_eq!(incoming[0]["user_id"], aid);
    assert!(incoming[0]["handle"].is_string());
    assert_eq!(
        req(&app, Method::DELETE, &cancel, None, Some(&a)).await.0,
        StatusCode::NO_CONTENT
    );
    befriend(&app, &a, &b, &bh).await;
    let (status, opened) = req(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":bid})),
        Some(&a),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let conversation = opened["id"].as_str().expect("conversation");
    let path = format!("/v1/dms/{conversation}/messages");
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("port");
    let port = listener.local_addr().expect("address").port();
    let server_app = app.clone();
    let server = tokio::spawn(async move {
        axum::serve(listener, server_app).await.expect("server");
    });
    let url = format!("ws://127.0.0.1:{port}/v1/dms/{conversation}/events");
    for token in [None, Some(&c)] {
        let mut request = url.as_str().into_client_request().expect("request");
        if let Some(token) = token {
            request.headers_mut().insert(
                header::AUTHORIZATION,
                format!("Bearer {token}").parse().expect("header"),
            );
        }
        let Err(tokio_tungstenite::tungstenite::Error::Http(response)) =
            connect_async(request).await
        else {
            panic!("private subscription must fail")
        };
        assert_eq!(
            response.status(),
            if token.is_none() {
                StatusCode::UNAUTHORIZED
            } else {
                StatusCode::NOT_FOUND
            }
        );
    }
    let mut sa = connect(&url, &a).await;
    let mut sb = connect(&url, &b).await;
    for socket in [&mut sa, &mut sb] {
        let ready = next_kind(socket, "ready").await;
        assert_eq!(ready["conversation_id"], conversation);
        assert!(ready.get("channel_id").is_none());
    }
    // A different authorized channel can have the same UUID as a DM. Its
    // subscriber must not observe private IDs/actors through the shared code.
    let (_, isolated) = req(
        &app,
        Method::POST,
        "/v1/communities",
        Some(json!({"name":"DM isolation"})),
        Some(&c),
    )
    .await;
    let isolated_id = isolated["id"].as_str().expect("community");
    let (_, channel) = req(
        &app,
        Method::POST,
        &format!("/v1/communities/{isolated_id}/channels"),
        Some(json!({"name":"isolated","channel_kind":"text","position":0})),
        Some(&c),
    )
    .await;
    sqlx::query("UPDATE channels SET id=$1 WHERE id=$2")
        .bind(Uuid::parse_str(conversation).expect("id"))
        .bind(Uuid::parse_str(channel["id"].as_str().expect("channel")).expect("id"))
        .execute(&pool)
        .await
        .expect("equal ID fixture");
    let channel_url = format!(
        "ws://127.0.0.1:{port}/v1/communities/{isolated_id}/channels/{conversation}/events"
    );
    let mut isolated_socket = connect(&channel_url, &c).await;
    next_kind(&mut isolated_socket, "ready").await;
    let id = Uuid::from_u128(rand::random()).to_string();
    let input = json!({"body":"private 🎮 literal <b>text</b>","client_message_id":id});
    let (first, second) = tokio::join!(
        req(&app, Method::POST, &path, Some(input.clone()), Some(&a)),
        req(&app, Method::POST, &path, Some(input), Some(&a))
    );
    assert!(
        (first.0 == StatusCode::CREATED && second.0 == StatusCode::OK)
            || (second.0 == StatusCode::CREATED && first.0 == StatusCode::OK)
    );
    assert_eq!(first.1, second.1);
    assert_eq!(
        first.1["created_at_us"].as_i64().expect("microseconds") / 1000,
        first.1["created_at_ms"].as_i64().expect("milliseconds")
    );
    for socket in [&mut sa, &mut sb] {
        let event = next_kind(socket, "message_created").await;
        assert_eq!(event["message_id"], id);
        assert_eq!(event["conversation_id"], conversation);
        for private in ["body", "token", "channel_id"] {
            assert!(event.get(private).is_none());
        }
    }
    let marker_path = format!("/v1/communities/{isolated_id}/channels/{conversation}/messages");
    let (status, marker) = req(
        &app,
        Method::POST,
        &marker_path,
        Some(json!({"body":"channel marker"})),
        Some(&c),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    // If hubs were accidentally shared, the private event would precede this
    // marker in the same queue. This proves ordering without a quiet-time guess.
    timeout(Duration::from_secs(12), async {
        loop {
            if let WsMessage::Text(text) = isolated_socket
                .next()
                .await
                .expect("open channel")
                .expect("channel frame")
            {
                let event: Value = serde_json::from_str(&text).expect("JSON");
                assert_ne!(
                    event["message_id"], id,
                    "DM event leaked into an equal-ID community channel"
                );
                if event["type"] == "message_created" && event["message_id"] == marker["id"] {
                    break;
                }
            }
        }
    })
    .await
    .expect("channel marker after private writes");
    drop(isolated_socket);
    let item = format!("{path}/{id}");
    assert_eq!(
        req(&app, Method::GET, &item, None, Some(&c)).await.0,
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        req(
            &app,
            Method::POST,
            &path,
            Some(json!({"body":"collision","client_message_id":id})),
            Some(&b)
        )
        .await
        .0,
        StatusCode::NOT_FOUND
    );
    for invalid in ["invalid", "00000000-0000-0000-0000-000000000000"] {
        assert_eq!(
            req(
                &app,
                Method::POST,
                &path,
                Some(json!({"body":"invalid","client_message_id":invalid})),
                Some(&a)
            )
            .await
            .0,
            StatusCode::BAD_REQUEST
        );
    }
    // All edits/deletes now acquire the same pair locks as block/unfriend.
    let mut held = pool.begin().await.expect("lock transaction");
    sqlx::query("SELECT id FROM users WHERE id IN ($1,$2) ORDER BY id FOR UPDATE")
        .bind(Uuid::parse_str(&aid).expect("id"))
        .bind(Uuid::parse_str(&bid).expect("id"))
        .fetch_all(&mut *held)
        .await
        .expect("lock pair");
    let edit_app = app.clone();
    let edit_item = item.clone();
    let edit_access = a.clone();
    let mut edit = tokio::spawn(async move {
        req(
            &edit_app,
            Method::PATCH,
            &edit_item,
            Some(json!({"body":"edited privately"})),
            Some(&edit_access),
        )
        .await
    });
    assert!(
        timeout(Duration::from_millis(150), &mut edit)
            .await
            .is_err(),
        "edit must wait for pair lock"
    );
    held.commit().await.expect("unlock");
    assert_eq!(edit.await.expect("edit task").0, StatusCode::OK);
    assert_eq!(next_kind(&mut sb, "message_edited").await["message_id"], id);
    let (_, retried) = req(
        &app,
        Method::POST,
        &path,
        Some(json!({"body":"must not overwrite","client_message_id":id})),
        Some(&a),
    )
    .await;
    assert_eq!(retried["body"], "edited privately");
    assert_eq!(
        req(&app, Method::DELETE, &item, None, Some(&b)).await.0,
        StatusCode::NOT_FOUND
    );
    assert_eq!(
        req(&app, Method::DELETE, &item, None, Some(&a)).await.0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        next_kind(&mut sb, "message_deleted").await["message_id"],
        id
    );
    let (_, tombstone) = req(&app, Method::GET, &item, None, Some(&b)).await;
    assert!(tombstone["body"].is_null());
    let (_, retried) = req(
        &app,
        Method::POST,
        &path,
        Some(json!({"body":"cannot resurrect","client_message_id":id})),
        Some(&a),
    )
    .await;
    assert!(retried["body"].is_null());
    sb.close(None).await.expect("close subscriber");
    let (_, missed) = req(
        &app,
        Method::POST,
        &path,
        Some(json!({"body":"missed while offline"})),
        Some(&a),
    )
    .await;
    let missed_item = format!("{path}/{}", missed["id"].as_str().expect("id"));
    assert_eq!(
        req(
            &app,
            Method::PATCH,
            &missed_item,
            Some(json!({"body":"updated while offline"})),
            Some(&a)
        )
        .await
        .0,
        StatusCode::OK
    );
    let mut sb = connect(&url, &b).await;
    next_kind(&mut sb, "ready").await;
    let (_, page) = req(&app, Method::GET, &path, None, Some(&b)).await;
    assert_eq!(page["messages"].as_array().expect("messages").len(), 2);
    assert_eq!(page["messages"][0]["body"], "updated while offline");
    assert_eq!(
        req(
            &app,
            Method::POST,
            "/v1/blocks",
            Some(json!({"user_id":aid})),
            Some(&b)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    closes(&mut sa).await;
    closes(&mut sb).await;
    for token in [&a, &b] {
        for method in [Method::GET, Method::POST, Method::PATCH, Method::DELETE] {
            let target = if method == Method::PATCH || method == Method::DELETE {
                &missed_item
            } else {
                &path
            };
            let body = if method == Method::POST || method == Method::PATCH {
                Some(json!({"body":"forbidden"}))
            } else {
                None
            };
            assert_eq!(
                req(&app, method, target, body, Some(token)).await.0,
                StatusCode::NOT_FOUND
            );
        }
    }
    assert_eq!(
        req(
            &app,
            Method::DELETE,
            &format!("/v1/blocks/{aid}"),
            None,
            Some(&b)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        req(&app, Method::GET, &path, None, Some(&a)).await.0,
        StatusCode::NOT_FOUND
    );
    befriend(&app, &a, &b, &bh).await;
    let (_, reopened) = req(
        &app,
        Method::POST,
        "/v1/dms",
        Some(json!({"user_id":bid})),
        Some(&a),
    )
    .await;
    assert_eq!(reopened["id"], conversation);
    let mut sa = connect(&url, &a).await;
    let mut sb = connect(&url, &b).await;
    next_kind(&mut sa, "ready").await;
    next_kind(&mut sb, "ready").await;
    assert_eq!(
        req(
            &app,
            Method::DELETE,
            &format!("/v1/friends/{bid}"),
            None,
            Some(&a)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    closes(&mut sa).await;
    closes(&mut sb).await;
    let mut refused = url.as_str().into_client_request().expect("request");
    refused.headers_mut().insert(
        header::AUTHORIZATION,
        format!("Bearer {a}").parse().expect("header"),
    );
    assert!(matches!(
        connect_async(refused).await,
        Err(tokio_tungstenite::tungstenite::Error::Http(_))
    ));
    befriend(&app, &a, &b, &bh).await;
    let mut sa = connect(&url, &a).await;
    let mut sb = connect(&url, &b).await;
    next_kind(&mut sa, "ready").await;
    next_kind(&mut sb, "ready").await;
    assert_eq!(
        req(&app, Method::POST, "/v1/auth/logout", None, Some(&a))
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    let (_, after_logout) = req(
        &app,
        Method::POST,
        &path,
        Some(json!({"body":"not for the revoked session"})),
        Some(&b),
    )
    .await;
    assert_eq!(
        next_kind(&mut sb, "message_created").await["message_id"],
        after_logout["id"]
    );
    timeout(Duration::from_secs(3), async {
        loop {
            match sa.next().await {
                None | Some(Ok(WsMessage::Close(_))) | Some(Err(_)) => break,
                Some(Ok(WsMessage::Text(text))) => {
                    let event: Value = serde_json::from_str(&text).expect("JSON");
                    assert_ne!(
                        event["message_id"], after_logout["id"],
                        "revoked session observed fresh private activity"
                    );
                }
                _ => {}
            }
        }
    })
    .await
    .expect("session revocation closes before fresh activity");
    assert_eq!(
        req(
            &app,
            Method::DELETE,
            &format!("/v1/communities/{isolated_id}"),
            None,
            Some(&c)
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    sqlx::query("DELETE FROM dm_conversations WHERE id=$1")
        .bind(Uuid::parse_str(conversation).expect("id"))
        .execute(&pool)
        .await
        .expect("cleanup");
    for id in [aid, bid, cid] {
        sqlx::query("DELETE FROM users WHERE id=$1")
            .bind(Uuid::parse_str(&id).expect("id"))
            .execute(&pool)
            .await
            .expect("cleanup");
    }
    server.abort();
}
