//! End-to-end native WebSocket/HTTP integration with PostgreSQL.
use std::time::Duration;

use axum::{
    Router,
    body::{Body, to_bytes},
    http::{Method, Request, StatusCode, header},
};
use futures_util::{SinkExt, StreamExt};
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
    Some(PgPoolOptions::new().max_connections(8).connect(&url).await.expect("CI PostgreSQL"))
}

async fn req(app: &Router, method: Method, url: &str,
    body: Option<Value>, token: Option<&str>) -> (StatusCode, Value)
{
    let mut builder = Request::builder().method(method).uri(url);
    if body.is_some() {
        builder = builder.header(header::CONTENT_TYPE, "application/json");
    }
    if let Some(token) = token {
        builder = builder.header(header::AUTHORIZATION, format!("Bearer {token}"));
    }
    let response = app.clone().oneshot(builder.body(body.map_or_else(
        Body::empty, |v| Body::from(v.to_string()))).expect("request")).await.expect("router");
    let status = response.status();
    let bytes = to_bytes(response.into_body(), 16384).await.expect("body");
    let result = if bytes.is_empty() { Value::Null } else {
        serde_json::from_slice(&bytes).expect("JSON")
    };
    (status, result)
}

async fn signup(app: &Router, prefix: &str) -> (String, String) {
    let suffix = format!("{:016x}", rand::random::<u64>());
    let handle = format!("{prefix}_{suffix}");
    let (status, result) = req(app, Method::POST, "/v1/auth/register",
        Some(json!({
            "handle":handle,"display_name":"Socket Test",
            "email":format!("{handle}@example.test"),
            "password":"this-is-a-long-test-password-2026"
        })), None).await;
    assert_eq!(status, StatusCode::CREATED);
    (
        result["access_token"].as_str().expect("token").to_owned(),
        result["user"]["id"].as_str().expect("id").to_owned(),
    )
}

async fn next_kind<S>(socket: &mut S, kind: &str) -> Value
where
    S: futures_util::Stream<Item = Result<WsMessage, tokio_tungstenite::tungstenite::Error>> + Unpin,
{
    timeout(Duration::from_secs(12), async {
        loop {
            let message = socket.next().await.expect("open").expect("socket response");
            if let WsMessage::Text(s) = message {
                let v: Value = serde_json::from_str(&s).expect("JSON frame");
                if v["type"] == kind { return v; }
            }
        }
    }).await.expect("expected event")
}

#[tokio::test]
async fn authorized_websocket_delivers_message_ids_and_rejects_removed_users() {
    let Some(pool) = db().await else { return };
    MIGRATOR.run(&pool).await.expect("migrations");
    let app = router_with_database(Some(pool.clone()));
    let (owner, owner_id) = signup(&app, "wsown").await;
    let (member, member_id) = signup(&app, "wsmem").await;
    let (outsider, outsider_id) = signup(&app, "wsout").await;
    let (status, community) = req(&app, Method::POST, "/v1/communities",
        Some(json!({"name":"Socket Security"})), Some(&owner)).await;
    assert_eq!(status, StatusCode::CREATED);
    let cid = community["id"].as_str().expect("community");
    let (status, channel) = req(&app, Method::POST,
        &format!("/v1/communities/{cid}/channels"),
        Some(json!({"name":"general","channel_kind":"text","position":0})),
        Some(&owner)).await;
    assert_eq!(status, StatusCode::CREATED);
    let chid = channel["id"].as_str().expect("channel");
    let (status, invite) = req(&app, Method::POST,
        &format!("/v1/communities/{cid}/invitations"), None, Some(&owner)).await;
    assert_eq!(status, StatusCode::CREATED);
    let (status, _) = req(&app, Method::POST, "/v1/invitations/accept",
        Some(json!({"token":invite["token"]})), Some(&member)).await;
    assert_eq!(status, StatusCode::CREATED);

    let listener = TcpListener::bind("127.0.0.1:0").await.expect("port");
    let port = listener.local_addr().expect("address").port();
    let server_app = app.clone(); // Exactly the same event hub as HTTP request service.
    let server = tokio::spawn(async move {
        axum::serve(listener, server_app).await.expect("server");
    });
    let ws_url = format!("ws://127.0.0.1:{port}/v1/communities/{cid}/channels/{chid}/events");
    let anonymous = ws_url.as_str().into_client_request().expect("request");
    assert!(matches!(connect_async(anonymous).await,
        Err(tokio_tungstenite::tungstenite::Error::Http(_))));

    let mut forbidden = ws_url.as_str().into_client_request().expect("request");
    forbidden.headers_mut().insert(header::AUTHORIZATION,
        format!("Bearer {outsider}").parse().expect("header"));
    if let Err(tokio_tungstenite::tungstenite::Error::Http(resp)) = connect_async(forbidden).await {
        assert_eq!(resp.status(), StatusCode::NOT_FOUND);
    } else {
        panic!("outsider must not subscribe");
    }

    let mut allowed = ws_url.as_str().into_client_request().expect("request");
    allowed.headers_mut().insert(header::AUTHORIZATION,
        format!("Bearer {member}").parse().expect("header"));
    let (mut socket, _) = connect_async(allowed).await.expect("member connected");
    assert_eq!(next_kind(&mut socket, "ready").await["channel_id"], chid);

    let path = format!("/v1/communities/{cid}/channels/{chid}/messages");
    let (status, created) = req(&app, Method::POST, &path,
        Some(json!({"body":"Secret message text"})), Some(&owner)).await;
    assert_eq!(status, StatusCode::CREATED);
    let msg_id = created["id"].as_str().expect("message");
    let notification = next_kind(&mut socket, "message_created").await;
    assert_eq!(notification["message_id"], msg_id);
    assert_eq!(notification["channel_id"], chid);
    assert!(notification.get("body").is_none(), "do not broadcast private bodies");
    let item = format!("{path}/{msg_id}");
    let (status, stored) = req(&app, Method::GET, &item, None, Some(&member)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(stored["body"], "Secret message text");

    let (status, _) = req(&app, Method::PATCH, &item,
        Some(json!({"body":"Edited text"})), Some(&owner)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(next_kind(&mut socket, "message_edited").await["message_id"], msg_id);

    socket.send(WsMessage::Text(json!({"type":"typing","active":true}).to_string().into()))
        .await.expect("typing command");
    let typed = next_kind(&mut socket, "typing").await;
    assert_eq!(typed["user_id"], member_id);
    assert_eq!(typed["active"], true);
    assert!(typed.get("body").is_none());

    let (status, _) = req(&app, Method::DELETE, &item, None, Some(&owner)).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    assert_eq!(next_kind(&mut socket, "message_deleted").await["message_id"], msg_id);

    // The socket must lose access after member removal. On the next
    // notification the server re-checks membership before delivery.
    let remove = format!("/v1/communities/{cid}/members/{member_id}");
    let (status, _) = req(&app, Method::DELETE, &remove, None, Some(&owner)).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    let (status, _) = req(&app, Method::POST, &path,
        Some(json!({"body":"do not show former member"})), Some(&owner)).await;
    assert_eq!(status, StatusCode::CREATED);
    let closed = timeout(Duration::from_secs(12), async {
        loop {
            match socket.next().await {
                None | Some(Ok(WsMessage::Close(_))) | Some(Err(_)) => break,
                Some(Ok(WsMessage::Text(text))) => {
                    let json: Value = serde_json::from_str(&text).expect("event JSON");
                    assert_ne!(json["type"], "message_created",
                        "removed member may not see fresh message event");
                }
                _ => {}
            }
        }
    }).await;
    assert!(closed.is_ok(), "revoked member WebSocket should be closed");

    let mut refused = ws_url.as_str().into_client_request().expect("request");
    refused.headers_mut().insert(header::AUTHORIZATION,
        format!("Bearer {member}").parse().expect("header"));
    assert!(matches!(connect_async(refused).await,
        Err(tokio_tungstenite::tungstenite::Error::Http(_))));

    let (status, _) = req(&app, Method::DELETE,
        &format!("/v1/communities/{cid}"), None, Some(&owner)).await;
    assert_eq!(status, StatusCode::NO_CONTENT);
    for id in [owner_id, member_id, outsider_id] {
        sqlx::query("DELETE FROM users WHERE id = $1")
            .bind(Uuid::parse_str(&id).expect("UUID"))
            .execute(&pool).await.expect("cleanup");
    }
    server.abort();
}
