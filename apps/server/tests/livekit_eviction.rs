//! HTTP/SQL integration fixtures; no real SFU, media or desktop is used here.
use std::{collections::BTreeMap, sync::Arc, time::{SystemTime, UNIX_EPOCH}};
use axum::{Router, Json, extract::{Path, State}, http::{HeaderMap, StatusCode}, response::{IntoResponse, Response}};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use sha2::{Sha256, Digest};
use serde_json::{Value, json};
use sqlx::{PgPool, postgres::PgPoolOptions, types::Uuid};
use tokio::sync::Mutex;
use gaming_comms_server::{database::MIGRATOR, livekit_tokens::LiveKitConfig, router_with_livekit};
use tower::ServiceExt;

const KEY: &str = "test-eviction-key";
const SECRET: &str = "local-fixture-secret-longer-than-thirty-two-bytes";
fn now() -> u64 { SystemTime::now().duration_since(UNIX_EPOCH).expect("clock").as_secs() }
fn identity(id: Uuid) -> String { format!("gcp-session-{id}") }
fn room(id: Uuid) -> String { format!("gcp-voice-{id}") }
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
async fn rpc(State(state): State<Arc<Mutex<Mock>>>, Path(method): Path<String>, headers: HeaderMap, Json(body): Json<Value>) -> Response {
    let jwt = headers.get("authorization").expect("authorization").to_str().expect("header").strip_prefix("Bearer ").expect("bearer");
    let parts: Vec<_> = jwt.split('.').collect();
    assert_eq!(parts.len(), 3);
    let mut mac = Hmac::<Sha256>::new_from_slice(SECRET.as_bytes()).expect("HMAC");
    mac.update(format!("{}.{}", parts[0], parts[1]).as_bytes());
    mac.verify_slice(&URL_SAFE_NO_PAD.decode(parts[2]).expect("signature encoding")).expect("signature");
    let claims: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(parts[1]).expect("claims encoding")).expect("claims");
    assert_eq!(claims["iss"], KEY);
    assert!(claims["exp"].as_u64().expect("expiry") <= now() + 30);
    assert!(claims["exp"].as_u64().expect("expiry") > now());
    assert!(claims["sub"].is_null());
    assert!(claims["video"]["roomJoin"].is_null());
    assert!(claims["video"]["canPublish"].is_null());
    if method == "ListRooms" {
        assert_eq!(claims["video"], json!({"roomList":true}));
    } else {
        assert_eq!(claims["video"], json!({"roomAdmin":true,"room":body["room"]}));
    }
    let mut mock = state.lock().await;
    if mock.redirect {
        return (StatusCode::FOUND, [("Location", "/unexpected")], "").into_response();
    }
    if mock.failure.as_deref() == Some(&method) {
        return (StatusCode::SERVICE_UNAVAILABLE, Json(json!({"code":"unavailable"}))).into_response();
    }
    if mock.oversized { return "x".repeat(256 * 1024 + 1).into_response(); }
    if mock.invalid_json { return Json(json!({"rooms":"bad"})).into_response(); }
    match method.as_str() {
        "ListRooms" => Json(json!({"rooms":mock.rooms.keys().map(|name| json!({"name":name})).collect::<Vec<_>>()})).into_response(),
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
            if let Some(people) = mock.rooms.get_mut(&name) { people.retain(|person| person != &id); }
            if mock.missing_response {
                (StatusCode::NOT_FOUND, Json(json!({"code":"not_found"}))).into_response()
            } else { Json(json!({})).into_response() }
        }
        _ => StatusCode::NOT_FOUND.into_response(),
    }
}
struct SfuFixture {
    state: Arc<Mutex<Mock>>,
    task: tokio::task::JoinHandle<()>,
    url: String,
}
impl Drop for SfuFixture { fn drop(&mut self) { self.task.abort(); } }
impl SfuFixture {
    async fn new() -> Self {
        let state = Arc::new(Mutex::new(Mock::default()));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.expect("listener");
        let url = format!("ws://{}", listener.local_addr().expect("address"));
        let router = Router::new().route("/twirp/livekit.RoomService/{method}", axum::routing::post(rpc))
            .route("/unexpected", axum::routing::post(|State(mock): State<Arc<Mutex<Mock>>>| async move {
                mock.lock().await.unexpected += 1;
                Json(json!({}))
            })).with_state(state.clone());
        let task = tokio::spawn(async move { axum::serve(listener, router).await.expect("SFU fixture"); });
        Self { state, task, url }
    }
    fn config(&self) -> LiveKitConfig { LiveKitConfig::new(self.url.clone(), KEY.into(), SECRET.into()).expect("config") }
}
async fn call(app: &Router, method: &str, path: &str, access: Option<&str>, body: Option<Value>) -> (StatusCode, Value) {
    let mut request = axum::http::Request::builder().method(method).uri(path);
    if let Some(token) = access { request = request.header("authorization", format!("Bearer {token}")); }
    if body.is_some() { request = request.header("content-type", "application/json"); }
    let response = app.clone().oneshot(request.body(body.map_or_else(axum::body::Body::empty, |v| axum::body::Body::from(v.to_string()))).expect("request")).await.expect("response");
    let status = response.status();
    let bytes = axum::body::to_bytes(response.into_body(), 16384).await.expect("body");
    (status, if bytes.is_empty() { Value::Null } else { serde_json::from_slice(&bytes).expect("JSON") })
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
    let pool = PgPoolOptions::new().connect_lazy("postgres://unused:unused@localhost/unused").expect("lazy pool");
    // No real DB query is needed for an empty room list or malformed HTTP data.
    config.reconcile_voice_access(&pool).await.expect("empty room snapshot");
    sfu.state.lock().await.redirect = true;
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    assert_eq!(sfu.state.lock().await.unexpected, 0);
    { let mut state = sfu.state.lock().await; state.redirect = false; state.oversized = true; }
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    { let mut state = sfu.state.lock().await; state.oversized = false; state.invalid_json = true; }
    assert!(config.reconcile_voice_access(&pool).await.is_err());
}

#[tokio::test]
async fn revoked_sessions_memberships_and_deleted_channels_are_evicted_and_retried() {
    let Ok(url) = std::env::var("GCP_TEST_DATABASE_URL") else { return; };
    let pool = PgPoolOptions::new().max_connections(5).connect(&url).await.expect("PostgreSQL");
    MIGRATOR.run(&pool).await.expect("migrations");
    let sfu = SfuFixture::new().await;
    let config = sfu.config();
    let app = router_with_livekit(Some(pool.clone()), Some(config.clone()));
    let owner = user(&pool).await;
    let member = user(&pool).await;
    let (owner_session, owner_token, _) = session(&pool, owner).await;
    let (first, first_token, _) = session(&pool, member).await;
    let (second, _second_token, second_refresh) = session(&pool, member).await;
    let (status, community) = call(&app,"POST","/v1/communities",Some(&owner_token),Some(json!({"name":"Eviction Test"}))).await;
    assert_eq!(status, StatusCode::CREATED);
    let cid = community["id"].as_str().expect("community");
    let community_id = Uuid::parse_str(cid).expect("UUID");
    sqlx::query("INSERT INTO community_memberships(community_id,user_id,role) VALUES($1,$2,'member')")
        .bind(community_id).bind(member).execute(&pool).await.expect("member");
    let (status, channel) = call(&app,"POST",&format!("/v1/communities/{cid}/channels"),Some(&owner_token),Some(json!({"name":"voice","channel_kind":"voice","position":0}))).await;
    assert_eq!(status, StatusCode::CREATED);
    let channel_id = Uuid::parse_str(channel["id"].as_str().expect("channel")).expect("UUID");
    let name = room(channel_id);
    let token_path = format!("/v1/communities/{cid}/channels/{channel_id}/livekit-token");
    assert_eq!(call(&app,"POST",&token_path,Some(&first_token),None).await.0, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(call(&app,"GET","/ready",None,None).await.0, StatusCode::SERVICE_UNAVAILABLE);
    { let mut state = sfu.state.lock().await;
      state.rooms.insert(name.clone(),vec![identity(first),identity(second),identity(owner_session),member.to_string()]);
      state.rooms.insert("unrelated-room".into(),vec!["external-client".into()]); }
    let report = config.reconcile_voice_access(&pool).await.expect("initial reconcile");
    assert_eq!(report.removed,1); // legacy user-only identity cannot prove a session
    assert_eq!(report.rooms_checked,1);
    assert_eq!(call(&app,"GET","/ready",None,None).await.0, StatusCode::NO_CONTENT);
    let (status, grant) = call(&app,"POST",&token_path,Some(&first_token),None).await;
    assert_eq!(status, StatusCode::CREATED);
    let jwt = grant["participant_token"].as_str().expect("JWT");
    let claims: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(jwt.split('.').nth(1).expect("claims")).expect("base64")).expect("JSON");
    assert_eq!(claims["sub"], identity(first));
    // Rotation keeps the same session identity and does not eject a valid login.
    let (status, rotated) = call(&app,"POST","/v1/auth/refresh",None,Some(json!({"refresh_token":second_refresh}))).await;
    assert_eq!(status, StatusCode::OK);
    let second_token = rotated["access_token"].as_str().expect("rotated token");
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("rotation reconcile").removed,0);
    let (expired, _, _) = session(&pool, member).await;
    sqlx::query("UPDATE user_sessions SET access_expires_at=now()-interval '1 second' WHERE id=$1")
        .bind(expired).execute(&pool).await.expect("expire session");
    sfu.state.lock().await.rooms.get_mut(&name).expect("room").push(identity(expired));
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("expiry eviction").removed,1);
    sqlx::query("DELETE FROM user_sessions WHERE id=$1").bind(expired).execute(&pool).await.expect("delete session");
    sfu.state.lock().await.rooms.get_mut(&name).expect("room").push(identity(expired));
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("deleted session eviction").removed,1);
    assert_eq!(call(&app,"POST","/v1/auth/logout",Some(&first_token),None).await.0,StatusCode::NO_CONTENT);
    sfu.state.lock().await.failure = Some("RemoveParticipant".into());
    assert!(config.reconcile_voice_access(&pool).await.is_err());
    assert_eq!(call(&app,"GET","/ready",None,None).await.0,StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(call(&app,"POST",&token_path,Some(second_token),None).await.0,StatusCode::SERVICE_UNAVAILABLE);
    { let mut state = sfu.state.lock().await; state.failure = None; state.missing_response = true; }
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("retry/concurrent leave").removed,1);
    sfu.state.lock().await.missing_response = false;
    assert!(sfu.state.lock().await.rooms[&name].contains(&identity(second)));
    // A modified client rejoining with an old self-hosted JWT is removed again.
    sfu.state.lock().await.rooms.get_mut(&name).expect("room").push(identity(first));
    let restarted = sfu.config();
    assert_eq!(restarted.reconcile_voice_access(&pool).await.expect("restart/replay").removed,1);
    assert_eq!(call(&app,"DELETE",&format!("/v1/communities/{cid}/members/{member}"),Some(&owner_token),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("member removed").removed,1);
    assert_eq!(call(&app,"DELETE",&format!("/v1/communities/{cid}/channels/{channel_id}"),Some(&owner_token),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("channel removed").removed,1);
    assert_eq!(sfu.state.lock().await.rooms["unrelated-room"],vec!["external-client"]);
    let (status, another) = call(&app,"POST",&format!("/v1/communities/{cid}/channels"),Some(&owner_token),Some(json!({"name":"another-voice","channel_kind":"voice","position":1}))).await;
    assert_eq!(status, StatusCode::CREATED);
    let another_id = Uuid::parse_str(another["id"].as_str().expect("another channel")).expect("UUID");
    sfu.state.lock().await.rooms.insert(room(another_id), vec![identity(owner_session)]);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("second channel").removed,0);
    assert_eq!(call(&app,"DELETE",&format!("/v1/communities/{cid}"),Some(&owner_token),None).await.0,StatusCode::NO_CONTENT);
    assert_eq!(config.reconcile_voice_access(&pool).await.expect("community deleted").removed,1);
    sqlx::query("DELETE FROM users WHERE id=ANY($1)").bind(vec![owner,member]).execute(&pool).await.expect("cleanup");
}
