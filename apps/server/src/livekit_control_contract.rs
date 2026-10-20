//! Real RoomService contract only; no participant connection or media claim.
use super::*;

const KEY: &str = "gcp-contract-key";
const SECRET: &str = "disposable-loopback-contract-secret-32bytes";
const URL: &str = "ws://127.0.0.1:17880";

async fn request(config: &LiveKitConfig, method: &str, video: Value, body: Value) -> reqwest::Response {
    let now = unix_seconds().expect("clock");
    let token = config
        .sign_claims(json!({"iss": KEY, "nbf": now - 2, "exp": now + 30, "video": video}))
        .expect("fixture JWT");
    let mut url = config.control_url.clone();
    url.set_path(&format!("/twirp/livekit.RoomService/{method}"));
    config
        .http
        .post(url)
        .bearer_auth(token)
        .json(&body)
        .send()
        .await
        .expect("real LiveKit control response")
}

#[tokio::test]
#[ignore = "requires scripts/test_livekit_control_contract.py disposable LiveKit server"]
async fn real_roomservice_contract() {
    let config = LiveKitConfig::new(URL.into(), KEY.into(), SECRET.into()).expect("config");
    let room = room_name(Uuid::from_u128(0x676370));
    let created = request(
        &config,
        "CreateRoom",
        json!({"roomCreate": true}),
        json!({"name": room, "empty_timeout": 60}),
    )
    .await;
    assert!(created.status().is_success(), "create: {}", created.status());
    let rooms: Rooms = config.rpc("ListRooms", None, json!({})).await.expect("list");
    assert!(rooms.rooms.iter().any(|item| item.name == room));
    let participants: Participants = config
        .rpc("ListParticipants", Some(&room), json!({"room": room}))
        .await
        .expect("room-scoped admin grant");
    assert!(participants.participants.is_empty());
    // Missing participants must produce the actual Twirp not_found response
    // accepted by the production idempotent-removal path, including its cutoff.
    let _: Value = config
        .rpc(
            "RemoveParticipant",
            Some(&room),
            json!({"room": room, "identity": "absent", "revoke_token_ts": unix_seconds().expect("clock") + 1}),
        )
        .await
        .expect("idempotent removal with revocation cutoff");
    let wrong_room = request(
        &config,
        "ListParticipants",
        json!({"roomAdmin": true, "room": "unrelated-room"}),
        json!({"room": room}),
    )
    .await;
    assert!(matches!(wrong_room.status().as_u16(), 401 | 403));
    let list_only = request(
        &config,
        "ListParticipants",
        json!({"roomList": true}),
        json!({"room": room}),
    )
    .await;
    assert!(matches!(list_only.status().as_u16(), 401 | 403));
    let invalid = LiveKitConfig::new(URL.into(), KEY.into(), "wrong-secret-longer-than-32-bytes!!".into())
        .expect("invalid signing key fixture");
    assert!(invalid.rpc::<Rooms>("ListRooms", None, json!({})).await.is_err());
    // Empty rooms exercise the production snapshot without requiring SQL access.
    let pool = sqlx::postgres::PgPoolOptions::new()
        .connect_lazy("postgres://unused:unused@127.0.0.1:1/unused")
        .expect("lazy pool");
    let report = config.reconcile_voice_access(&pool).await.expect("snapshot");
    assert_eq!(report.rooms_checked, 1);
    assert_eq!(report.participants_checked, 0);
    assert!(config.eviction_healthy());
    let _: Value = config
        .rpc("DeleteRoom", Some(&room), json!({"room": room}))
        .await
        .expect("cleanup room");
}
