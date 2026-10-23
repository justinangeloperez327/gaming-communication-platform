//! HTTP/SQL integration fixtures; no real SFU, media or desktop is used here.
use axum::{
    Json, Router,
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use gaming_comms_server::{database::MIGRATOR, livekit_tokens::LiveKitConfig, router_with_livekit};
use hmac::{Hmac, Mac};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use std::{
    collections::BTreeMap,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::sync::Mutex;
use tower::ServiceExt;

const KEY: &str = "test-eviction-key";
const SECRET: &str = "local-fixture-secret-longer-than-thirty-two-bytes";
fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("clock")
        .as_secs()
}
fn identity(id: Uuid) -> String {
    format!("gcp-session-{id}")
}
fn room(id: Uuid) -> String {
    format!("gcp-voice-{id}")
}
#[derive(Default)]
struct Mock {
    rooms: BTreeMap<String, Vec<String>>,
    removed: Vec<(String, String)>,
    failure: Option<String>,
    invalid_json: bool,
    oversized: bool,
    redirect: bool,
    missing_response: bool,
    unexpected: usize,
}
async fn rpc(
    State(state): State<Arc<Mutex<Mock>>>,
    Path(method): Path<String>,
    headers: HeaderMap,
    Json(body): Json<Value>,
) -> Response {
    let jwt = headers
        .get("authorization")
        .expect("authorization")
        .to_str()
        .expect("header")
        .strip_prefix("Bearer ")
        .expect("bearer");
    let parts: Vec<_> = jwt.split('.').collect();
    assert_eq!(parts.len(), 3);
    let mut mac = Hmac::<Sha256>::new_from_slice(SECRET.as_bytes()).expect("HMAC");
    mac.update(format!("{}.{}", parts[0], parts[1]).as_bytes());
    mac.verify_slice(
        &URL_SAFE_NO_PAD
            .decode(parts[2])
            .expect("signature encoding"),
    )
    .expect("signature");
    let claims: Value =
        serde_json::from_slice(&URL_SAFE_NO_PAD.decode(parts[1]).expect("claims encoding"))
            .expect("claims");
    assert_eq!(claims["iss"], KEY);
    assert!(claims["exp"].as_u64().expect("expiry") <= now() + 30);
    assert!(claims["exp"].as_u64().expect("expiry") > now());
    assert!(claims["sub"].is_null());
    assert!(claims["video"]["roomJoin"].is_null());
    assert!(claims["video"]["canPublish"].is_null());
    if method == "ListRooms" {
        assert_eq!(claims["video"], json!({"roomList":true}));
    } else {
        assert_eq!(
            claims["video"],
            json!({"roomAdmin":true,"room":body["room"]})
        );
    }
    let mut mock = state.lock().await;
    if mock.redirect {
        return (StatusCode::FOUND, [("Location", "/unexpected")], "").into_response();
    }
    if mock.failure.as_deref() == Some(&method) {
        return (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(json!({"code":"unavailable"})),
        )
            .into_response();
    }
    if mock.oversized {
        return "x".repeat(256 * 1024 + 1).into_response();
    }
    if mock.invalid_json {
        return Json(json!({"rooms":"bad"})).into_response();
    }
    match method.as_str() {
        "ListRooms" => Json(
            json!({"rooms":mock.rooms.keys().map(|name| json!({"name":name})).collect::<Vec<_>>()}),
        )
        .into_response(),
        "ListParticipants" => {
            let name = body["room"].as_str().expect("room");
            Json(json!({"participants":mock.rooms.get(name).into_iter().flatten().map(|id| json!({"identity":id})).collect::<Vec<_>>()})).into_response()
        }
        "RemoveParticipant" => {
            let name = body["room"].as_str().expect("room").to_string();
            let id = body["identity"].as_str().expect("identity").to_string();
            let cutoff = body["revoke_token_ts"].as_u64().expect("explicit cutoff");
            assert!(cutoff.abs_diff(now()) <= 2);
            mock.removed.push((name.clone(), id.clone()));
            if let Some(people) = mock.rooms.get_mut(&name) {
                people.retain(|person| person != &id);
            }
            if mock.missing_response {
                (StatusCode::NOT_FOUND, Json(json!({"code":"not_found"}))).into_response()
            } else {
                Json(json!({})).into_response()
            }
        }
        _ => StatusCode::NOT_FOUND.into_response(),
    }
}
struct SfuFixture {
    state: Arc<Mutex<Mock>>,
    task: tokio::task::JoinHandle<()>,
    url: String,
}
impl Drop for SfuFixture {
    fn drop(&mut self) {
        self.task.abort();
    }
}
impl SfuFixture {
    async fn new() -> Self {
        let state = Arc::new(Mutex::new(Mock::default()));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
            .await
            .expect("listener");
        let url = format!("ws://{}", listener.local_addr().expect("address"));
        let router = Router::new()
            .route(
                "/twirp/livekit.RoomService/{method}",
                axum::routing::post(rpc),
            )
            .route(
                "/unexpected",
                axum::routing::post(|State(mock): State<Arc<Mutex<Mock>>>| async move {
                    mock.lock().await.unexpected += 1;
                    Json(json!({}))
                }),
            )
            .with_state(state.clone());
        let task = tokio::spawn(async move {
            axum::serve(listener, router).await.expect("SFU fixture");
        });
        Self { state, task, url }
    }
    fn config(&self) -> LiveKitConfig {
        LiveKitConfig::new(self.url.clone(), KEY.into(), SECRET.into()).expect("config")
    }
}
async fn call(
    app: &Router,
    method: &str,
    path: &str,
    access: Option<&str>,
    body: Option<Value>,
) -> (StatusCode, Value) {
    let mut request = axum::http::Request::builder().method(method).uri(path);
    if let Some(token) = access {
        request = request.header("authorization", format!("Bearer {token}"));
    }
    if body.is_some() {
        request = request.header("content-type", "application/json");
    }
    let response = app
        .clone()
        .oneshot(
            request
                .body(body.map_or_else(axum::body::Body::empty, |v| {
                    axum::body::Body::from(v.to_string())
                }))
                .expect("request"),
        )
        .await
        .expect("response");
    let status = response.status();
    assert_eq!(response.headers()["cache-control"], "no-store");
    let bytes = axum::body::to_bytes(response.into_body(), 16384)
        .await
        .expect("body");
    (
        status,
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(&bytes).expect("JSON")
        },
    )
}
async fn user(pool: &PgPool) -> Uuid {
    let handle = format!("ev_{:032x}", rand::random::<u128>());
    // UUID text needs truncation to the schema's 32-character handle bound.
    let handle = &handle[..30];
    sqlx::query_scalar("INSERT INTO users(handle,display_name,email,password_hash) VALUES($1,'Eviction Test',$2,'unused-testing-password-fixture') RETURNING id")
        .bind(handle).bind(format!("{handle}@example.test")).fetch_one(pool).await.expect("user")
}
async fn session(pool: &PgPool, user: Uuid) -> (Uuid, String, String) {
    let access = format!("{:064x}", rand::random::<u128>());
    let refresh = format!("{:064x}", rand::random::<u128>());
    let digest = |value: &str| format!("{:x}", Sha256::digest(value.as_bytes()));
    let id = sqlx::query_scalar("INSERT INTO user_sessions(user_id,access_hash,refresh_hash,access_expires_at,refresh_expires_at) VALUES($1,$2,$3,now()+interval '15 minutes',now()+interval '30 days') RETURNING id")
        .bind(user).bind(digest(&access)).bind(digest(&refresh)).fetch_one(pool).await.expect("session");
    (id, access, refresh)
}

#[tokio::test]
async fn control_transport_fails_closed_and_does_not_follow_redirects() {
    let sfu = SfuFixture::new().await;
    let config = sfu.config();
    let pool = PgPoolOptions::new()
        .connect_lazy("postgres://unused:unused@localhost/unused")
        .expect("lazy pool");
    // No real DB query is needed for an empty room list or malformed HTTP data.
    config
        .reconcile_voice_access(&pool)
        .await
        .expect("empty room snapshot");
    sfu.state.lock().await.redirect = true;
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    assert_eq!(sfu.state.lock().await.unexpected, 0);
    {
        let mut state = sfu.state.lock().await;
        state.redirect = false;
        state.oversized = true;
    }
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    {
        let mut state = sfu.state.lock().await;
        state.oversized = false;
        state.invalid_json = true;
    }
    assert!(config.reconcile_voice_access(&pool).await.is_err());
}

#[tokio::test]
async fn revoked_sessions_memberships_and_deleted_channels_are_evicted_and_retried() {
    let Ok(url) = std::env::var("GCP_TEST_DATABASE_URL") else {
        return;
    };
    let pool = PgPoolOptions::new()
        .max_connections(5)
        .connect(&url)
        .await
        .expect("PostgreSQL");
    MIGRATOR.run(&pool).await.expect("migrations");
    let sfu = SfuFixture::new().await;
    let config = sfu.config();
    let app = router_with_livekit(Some(pool.clone()), Some(config.clone()));
    let owner = user(&pool).await;
    let member = user(&pool).await;
    let (owner_session, owner_token, _) = session(&pool, owner).await;
    let (first, first_token, _) = session(&pool, member).await;
    let (second, _second_token, second_refresh) = session(&pool, member).await;
    let (status, community) = call(
        &app,
        "POST",
        "/v1/communities",
        Some(&owner_token),
        Some(json!({"name":"Eviction Test"})),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let cid = community["id"].as_str().expect("community");
    let community_id = Uuid::parse_str(cid).expect("UUID");
    sqlx::query(
        "INSERT INTO community_memberships(community_id,user_id,role) VALUES($1,$2,'member')",
    )
    .bind(community_id)
    .bind(member)
    .execute(&pool)
    .await
    .expect("member");
    let (status, channel) = call(
        &app,
        "POST",
        &format!("/v1/communities/{cid}/channels"),
        Some(&owner_token),
        Some(json!({"name":"voice","channel_kind":"voice","position":0})),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let channel_id = Uuid::parse_str(channel["id"].as_str().expect("channel")).expect("UUID");
    let name = room(channel_id);
    let token_path = format!("/v1/communities/{cid}/channels/{channel_id}/livekit-token");
    assert_eq!(
        call(&app, "POST", &token_path, Some(&first_token), None)
            .await
            .0,
        StatusCode::SERVICE_UNAVAILABLE
    );
    assert_eq!(
        call(&app, "GET", "/ready", None, None).await.0,
        StatusCode::SERVICE_UNAVAILABLE
    );
    {
        let mut state = sfu.state.lock().await;
        state.rooms.insert(
            name.clone(),
            vec![
                identity(first),
                identity(second),
                identity(owner_session),
                member.to_string(),
            ],
        );
        state
            .rooms
            .insert("unrelated-room".into(), vec!["external-client".into()]);
    }
    let report = config
        .reconcile_voice_access(&pool)
        .await
        .expect("initial reconcile");
    assert_eq!(report.removed, 1); // legacy user-only identity cannot prove a session
    assert_eq!(report.rooms_checked, 1);
    assert_eq!(
        call(&app, "GET", "/ready", None, None).await.0,
        StatusCode::NO_CONTENT
    );
    let (status, grant) = call(&app, "POST", &token_path, Some(&first_token), None).await;
    assert_eq!(status, StatusCode::CREATED);
    let jwt = grant["participant_token"].as_str().expect("JWT");
    let claims: Value = serde_json::from_slice(
        &URL_SAFE_NO_PAD
            .decode(jwt.split('.').nth(1).expect("claims"))
            .expect("base64"),
    )
    .expect("JSON");
    assert_eq!(claims["sub"], identity(first));
    // Rotation keeps the same session identity and does not eject a valid login.
    let (status, rotated) = call(
        &app,
        "POST",
        "/v1/auth/refresh",
        None,
        Some(json!({"refresh_token":second_refresh})),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let second_token = rotated["access_token"].as_str().expect("rotated token");
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("rotation reconcile")
            .removed,
        0
    );
    let (expired, _, _) = session(&pool, member).await;
    sqlx::query("UPDATE user_sessions SET access_expires_at=now()-interval '1 second' WHERE id=$1")
        .bind(expired)
        .execute(&pool)
        .await
        .expect("expire session");
    sfu.state
        .lock()
        .await
        .rooms
        .get_mut(&name)
        .expect("room")
        .push(identity(expired));
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("expiry eviction")
            .removed,
        1
    );
    sqlx::query("DELETE FROM user_sessions WHERE id=$1")
        .bind(expired)
        .execute(&pool)
        .await
        .expect("delete session");
    sfu.state
        .lock()
        .await
        .rooms
        .get_mut(&name)
        .expect("room")
        .push(identity(expired));
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("deleted session eviction")
            .removed,
        1
    );
    assert_eq!(
        call(&app, "POST", "/v1/auth/logout", Some(&first_token), None)
            .await
            .0,
        StatusCode::NO_CONTENT
    );
    sfu.state.lock().await.failure = Some("RemoveParticipant".into());
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    assert_eq!(
        call(&app, "GET", "/ready", None, None).await.0,
        StatusCode::SERVICE_UNAVAILABLE
    );
    assert_eq!(
        call(&app, "POST", &token_path, Some(second_token), None)
            .await
            .0,
        StatusCode::SERVICE_UNAVAILABLE
    );
    {
        let mut state = sfu.state.lock().await;
        state.failure = None;
        state.missing_response = true;
    }
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("retry/concurrent leave")
            .removed,
        1
    );
    sfu.state.lock().await.missing_response = false;
    assert!(sfu.state.lock().await.rooms[&name].contains(&identity(second)));
    // A modified client rejoining with an old self-hosted JWT is removed again.
    sfu.state
        .lock()
        .await
        .rooms
        .get_mut(&name)
        .expect("room")
        .push(identity(first));
    let restarted = sfu.config();
    assert_eq!(
        restarted
            .reconcile_voice_access(&pool)
            .await
            .expect("restart/replay")
            .removed,
        1
    );
    assert_eq!(
        call(
            &app,
            "DELETE",
            &format!("/v1/communities/{cid}/members/{member}"),
            Some(&owner_token),
            None
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("member removed")
            .removed,
        1
    );
    assert_eq!(
        call(
            &app,
            "DELETE",
            &format!("/v1/communities/{cid}/channels/{channel_id}"),
            Some(&owner_token),
            None
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("channel removed")
            .removed,
        1
    );
    assert_eq!(
        sfu.state.lock().await.rooms["unrelated-room"],
        vec!["external-client"]
    );
    let (status, another) = call(
        &app,
        "POST",
        &format!("/v1/communities/{cid}/channels"),
        Some(&owner_token),
        Some(json!({"name":"another-voice","channel_kind":"voice","position":1})),
    )
    .await;
    assert_eq!(status, StatusCode::CREATED);
    let another_id =
        Uuid::parse_str(another["id"].as_str().expect("another channel")).expect("UUID");
    sfu.state
        .lock()
        .await
        .rooms
        .insert(room(another_id), vec![identity(owner_session)]);
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("second channel")
            .removed,
        0
    );
    assert_eq!(
        call(
            &app,
            "DELETE",
            &format!("/v1/communities/{cid}"),
            Some(&owner_token),
            None
        )
        .await
        .0,
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        config
            .reconcile_voice_access(&pool)
            .await
            .expect("community deleted")
            .removed,
        1
    );
    sqlx::query("DELETE FROM users WHERE id=ANY($1)")
        .bind(vec![owner, member])
        .execute(&pool)
        .await
        .expect("cleanup");
}

fn screen_claims(grant: &Value, channel: Uuid, purpose: &str) -> String {
    let id = grant["id"].as_str().expect("lease ID");
    let expected_identity = format!("gcp-screen-{purpose}-{id}");
    let expected_room = format!("gcp-screen-{channel}");
    assert_eq!(grant["purpose"], purpose);
    assert_eq!(grant["participant_identity"], expected_identity);
    assert_eq!(grant["room"], expected_room);
    let parts: Vec<_> = grant["participant_token"].as_str().expect("JWT").split('.').collect();
    let mut mac = Hmac::<Sha256>::new_from_slice(SECRET.as_bytes()).expect("HMAC");
    mac.update(format!("{}.{}", parts[0], parts[1]).as_bytes());
    mac.verify_slice(&URL_SAFE_NO_PAD.decode(parts[2]).expect("signature"))
        .expect("actual server signature");
    let claims: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(parts[1]).expect("claims"))
        .expect("JSON claims");
    assert_eq!(claims["sub"], expected_identity);
    assert_eq!(claims["video"], json!({"roomJoin":true,"room":expected_room,
        "canPublish":purpose=="publish", "canPublishSources":if purpose=="publish" {json!(["screen_share"])} else {json!([])},
        "canSubscribe":purpose=="view","canPublishData":false,"canUpdateOwnMetadata":false}));
    let expiry = claims["exp"].as_u64().expect("expiry");
    assert!(expiry > now() && expiry <= now()+60);
    assert!(grant["expires_in"].as_u64().expect("lifetime")<=60);
    assert!(claims.get("email").is_none());
    expected_identity
}

#[tokio::test]
async fn screen_consent_leases_ownership_revocation_and_voice_coexistence() {
    let Ok(url) = std::env::var("GCP_TEST_DATABASE_URL") else { return };
    let pool = PgPoolOptions::new().max_connections(6).connect(&url).await.expect("PostgreSQL");
    MIGRATOR.run(&pool).await.expect("migrations");
    let sfu = SfuFixture::new().await;
    let config = sfu.config();
    let app = router_with_livekit(Some(pool.clone()), Some(config.clone()));
    let owner = user(&pool).await;
    let member = user(&pool).await;
    let outsider = user(&pool).await;
    let (_, owner_token, _) = session(&pool, owner).await;
    let (login, token, refresh) = session(&pool, member).await;
    let (_, other_login, _) = session(&pool, member).await;
    let (_, outsider_token, _) = session(&pool, outsider).await;
    let (status, community) = call(&app,"POST","/v1/communities",Some(&owner_token),
        Some(json!({"name":"Screen authorization"}))).await;
    assert_eq!(status, StatusCode::CREATED);
    let community = Uuid::parse_str(community["id"].as_str().expect("community")).expect("UUID");
    sqlx::query("INSERT INTO community_memberships(community_id,user_id,role) VALUES($1,$2,'member')")
        .bind(community).bind(member).execute(&pool).await.expect("member");
    let mut channels = Vec::new();
    for kind in ["voice","voice","text"] {
        let (_, channel) = call(&app,"POST",&format!("/v1/communities/{community}/channels"),
            Some(&owner_token), Some(json!({"name":format!("screen-{}",channels.len()),"channel_kind":kind,"position":channels.len()}))).await;
        channels.push(Uuid::parse_str(channel["id"].as_str().expect("channel")).expect("UUID"));
    }
    let channel = channels[0];
    let path = format!("/v1/communities/{community}/channels/{channel}/screen-sessions");
    let create = json!({"purpose":"publish","consent":true});
    assert_eq!(call(&app,"POST",&path,None,Some(create.clone())).await.0,StatusCode::UNAUTHORIZED);
    assert_eq!(call(&app,"POST",&path,Some(&outsider_token),Some(create.clone())).await.0,StatusCode::SERVICE_UNAVAILABLE);
    // Readiness blocks all creation; no lease is persisted before SFU health.
    assert_eq!(call(&app,"POST",&path,Some(&token),Some(create.clone())).await.0,StatusCode::SERVICE_UNAVAILABLE);
    config.reconcile_voice_access(&pool).await.expect("initial health");
    assert_eq!(call(&app,"POST",&path,Some(&outsider_token),Some(create.clone())).await.0,StatusCode::NOT_FOUND);
    for invalid in [json!({"purpose":"publish","consent":false}),json!({"purpose":"publish"}),
        json!({"purpose":"camera","consent":true}),json!({"purpose":"publish","consent":true,"identity":"chosen"})] {
        assert!(matches!(call(&app,"POST",&path,Some(&token),Some(invalid)).await.0.as_u16(),400|422));
    }
    for invalid_path in [format!("/v1/communities/{community}/channels/{}/screen-sessions",channels[2]),
        format!("/v1/communities/{outsider}/channels/{channel}/screen-sessions")] {
        assert_eq!(call(&app,"POST",&invalid_path,Some(&token),Some(create.clone())).await.0,StatusCode::NOT_FOUND);
    }
    // Simultaneous creates must yield exactly one lease, not identity takeover.
    let (first, second) = tokio::join!(call(&app,"POST",&path,Some(&token),Some(create.clone())),
        call(&app,"POST",&path,Some(&token),Some(create.clone())));
    let (grant, denied) = if first.0==StatusCode::CREATED {(first,second)} else {(second,first)};
    assert_eq!(grant.0,StatusCode::CREATED);
    assert_eq!(denied.0,StatusCode::CONFLICT);
    let publisher = screen_claims(&grant.1,channel,"publish");
    let lease = grant.1["id"].as_str().expect("lease");
    let lease_path = format!("{path}/{lease}");
    let renew_path = format!("{lease_path}/renew");
    let (_, viewer_grant) = call(&app,"POST",&path,Some(&token),Some(json!({"purpose":"view","consent":true}))).await;
    let viewer = screen_claims(&viewer_grant,channel,"view");
    assert_ne!(publisher,viewer);
    let voice_path = format!("/v1/communities/{community}/channels/{channel}/livekit-token");
    let (voice_status, voice_grant) = call(&app,"POST",&voice_path,Some(&token),None).await;
    assert_eq!(voice_status,StatusCode::CREATED);
    assert_eq!(voice_grant["room"],room(channel));
    let voice_claims: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(voice_grant["participant_token"]
        .as_str().expect("voice JWT").split('.').nth(1).expect("claims")).expect("base64")).expect("JSON");
    assert_eq!(voice_claims["sub"],identity(login));
    assert_eq!(voice_claims["video"]["canPublishSources"],json!(["microphone"]));
    let screen_room = format!("gcp-screen-{channel}");
    {
        let mut state = sfu.state.lock().await;
        state.rooms.insert(room(channel),vec![identity(login),publisher.clone()]);
        state.rooms.insert(screen_room.clone(),vec![publisher.clone(),viewer.clone(),identity(login),
            publisher.replace("-publish-","-view-")]);
        state.rooms.insert(format!("gcp-screen-{}",channels[1]),vec![publisher.clone()]);
        state.rooms.insert("operator-diagnostic-room".into(),vec!["operator-peer".into()]);
    }
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("scope and purpose isolation").removed,4);
    let (listed_status, listed) = call(&app,"GET",&path,Some(&owner_token),None).await;
    assert_eq!(listed_status,StatusCode::OK);
    assert_eq!(listed.as_array().expect("publishers").len(),1);
    assert_eq!(listed[0]["user_id"],member.to_string());
    assert_eq!(listed[0]["participant_identity"],publisher);
    assert!(listed[0].get("participant_token").is_none());
    assert!(listed[0].get("session_id").is_none());
    assert_eq!(call(&app,"GET",&path,Some(&outsider_token),None).await.0,StatusCode::NOT_FOUND);
    // Same account on another login and community owner cannot renew/stop it.
    for other in [&other_login,&owner_token,&outsider_token] {
        assert_eq!(call(&app,"POST",&renew_path,Some(other),Some(json!({"consent":true}))).await.0,StatusCode::NOT_FOUND);
        assert_eq!(call(&app,"DELETE",&lease_path,Some(other),None).await.0,StatusCode::NO_CONTENT);
    }
    assert!(sfu.state.lock().await.rooms[&screen_room].contains(&publisher));
    assert_eq!(call(&app,"POST",&renew_path,Some(&token),Some(json!({"consent":false}))).await.0,StatusCode::BAD_REQUEST);
    let (status, rotated) = call(&app,"POST","/v1/auth/refresh",None,Some(json!({"refresh_token":refresh}))).await;
    assert_eq!(status,StatusCode::OK);
    let fresh = rotated["access_token"].as_str().expect("rotated token");
    assert_eq!(call(&app,"POST",&renew_path,Some(&token),Some(json!({"consent":true}))).await.0,StatusCode::UNAUTHORIZED);
    let (status, renewed) = call(&app,"POST",&renew_path,Some(fresh),Some(json!({"consent":true}))).await;
    assert_eq!(status,StatusCode::OK);
    assert_eq!(screen_claims(&renewed,channel,"publish"),publisher);
    assert_ne!(renewed["participant_token"],grant.1["participant_token"]);
    // Lost control plane cannot prevent durable Stop; next pass removes only it.
    sfu.state.lock().await.failure=Some("RemoveParticipant".into());
    assert_eq!(call(&app,"DELETE",&lease_path,Some(fresh),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(call(&app,"POST",&path,Some(fresh),Some(create.clone())).await.0,StatusCode::SERVICE_UNAVAILABLE);
    sfu.state.lock().await.failure=None;
    let restarted = sfu.config();
    assert_eq!(restarted.reconcile_voice_access(&pool).await.expect("restart/stop retry").removed,1);
    assert_eq!(sfu.state.lock().await.rooms[&room(channel)],vec![identity(login)]);
    assert_eq!(sfu.state.lock().await.rooms[&screen_room],vec![viewer.clone()]);
    config.reconcile_voice_access(&pool).await.expect("health recovered");
    assert_eq!(call(&app,"POST",&renew_path,Some(fresh),Some(json!({"consent":true}))).await.0,StatusCode::NOT_FOUND);
    assert_eq!(call(&app,"DELETE",&lease_path,Some(fresh),None).await.0,StatusCode::NO_CONTENT);
    let (_, replacement) = call(&app,"POST",&path,Some(fresh),Some(create.clone())).await;
    let replacement_identity = screen_claims(&replacement,channel,"publish");
    assert_ne!(replacement_identity,publisher);
    // Expired leases cannot be renewed; a new explicit create gets a new ID.
    sqlx::query("UPDATE screen_leases SET expires_at=clock_timestamp()-interval '1 second' WHERE session_id=$1")
        .bind(login).execute(&pool).await.expect("expire leases");
    assert_eq!(call(&app,"POST",&format!("{path}/{}/renew",replacement["id"].as_str().expect("id")),
        Some(fresh),Some(json!({"consent":true}))).await.0,StatusCode::NOT_FOUND);
    sfu.state.lock().await.rooms.get_mut(&screen_room).expect("room").push(replacement_identity);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("expired publisher/viewer").removed,2);
    let (_, renewed_publisher) = call(&app,"POST",&path,Some(fresh),Some(create.clone())).await;
    let active = screen_claims(&renewed_publisher,channel,"publish");
    sfu.state.lock().await.rooms.get_mut(&screen_room).expect("room").push(active);
    // Membership removal revokes both scopes; Stop still works without membership.
    assert_eq!(call(&app,"DELETE",&format!("/v1/communities/{community}/members/{member}"),Some(&owner_token),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(call(&app,"POST",&format!("{path}/{}/renew",renewed_publisher["id"].as_str().expect("id")),Some(fresh),Some(json!({"consent":true}))).await.0,StatusCode::NOT_FOUND);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("membership revoked").removed,2);
    assert_eq!(call(&app,"DELETE",&format!("{path}/{}",renewed_publisher["id"].as_str().expect("id")),Some(fresh),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(sfu.state.lock().await.rooms["operator-diagnostic-room"],vec!["operator-peer"]);
    assert_eq!(call(&app,"POST",&path,Some(fresh),Some(create)).await.0,StatusCode::NOT_FOUND);
    sqlx::query("DELETE FROM communities WHERE id=$1").bind(community).execute(&pool).await.expect("cleanup community");
    sqlx::query("DELETE FROM users WHERE id=ANY($1)").bind(vec![owner,member,outsider]).execute(&pool).await.expect("cleanup users");
}
