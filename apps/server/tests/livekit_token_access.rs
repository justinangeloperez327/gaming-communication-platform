//! End-to-end voice-room token authorization with disposable PostgreSQL.
//! No real LiveKit server is contacted; JWT structure/signature is unit-tested.
use axum::{
    Router, body::{Body,to_bytes},
    http::{Method,Request,StatusCode,header},
};
use gaming_comms_server::{database::MIGRATOR,livekit_tokens::LiveKitConfig,router_with_livekit};
use serde_json::{Value,json};
use sqlx::{PgPool,postgres::PgPoolOptions,types::Uuid};
use tower::ServiceExt;

async fn connect() -> Option<PgPool> {
    let url=match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(v)=>v,Err(std::env::VarError::NotPresent)=>return None,
        Err(_)=>panic!("invalid test database URL"),
    };
    Some(PgPoolOptions::new().max_connections(8).connect(&url).await
        .expect("PostgreSQL service should exist in CI"))
}
async fn call(app:&Router, method:Method, path:&str, json:Option<Value>,
    access:Option<&str>) -> (StatusCode,Value,bool)
{
    let mut req=Request::builder().uri(path).method(method);
    if json.is_some() { req=req.header(header::CONTENT_TYPE,"application/json"); }
    if let Some(token)=access {
        req=req.header(header::AUTHORIZATION,format!("Bearer {token}"));
    }
    let resp=app.clone().oneshot(req.body(json.map_or_else(Body::empty, |v|
        Body::from(v.to_string()))).expect("request")).await.expect("router");
    let no_store=resp.headers().get(header::CACHE_CONTROL)
        .and_then(|v|v.to_str().ok()) == Some("no-store");
    let status=resp.status();
    let bytes=to_bytes(resp.into_body(),16384).await.expect("body");
    let body=if bytes.is_empty() { Value::Null }
        else {serde_json::from_slice(&bytes).expect("response JSON")};
    (status,body,no_store)
}
async fn signup(app:&Router,prefix:&str)->(String,String) {
    let rnd=format!("{:016x}",rand::random::<u64>());
    let handle=format!("{prefix}_{rnd}");
    let (status,body,_)=call(app,Method::POST,"/v1/auth/register",
        Some(json!({"handle":handle,"email":format!("{handle}@example.test"),
            "display_name":"Room Player","password":"strong-testing-password-2026"})),None).await;
    assert_eq!(status,StatusCode::CREATED);
    (body["access_token"].as_str().expect("access token").to_string(),
     body["user"]["id"].as_str().expect("id").to_string())
}

#[tokio::test]
async fn tokens_are_issued_only_for_active_voice_channel_members() {
    let Some(db)=connect().await else {return};
    MIGRATOR.run(&db).await.expect("migrations");
    let config=LiveKitConfig::new("wss://voice.example.test".into(),
        "test-issuer-key".into(),"test-secret-of-more-than-thirty-two-bytes".into())
        .expect("valid test credentials");
    let app=router_with_livekit(Some(db.clone()),Some(config));
    let (owner,owner_id)=signup(&app,"lkown").await;
    let (member,member_id)=signup(&app,"lkmem").await;
    let (outsider,outsider_id)=signup(&app,"lkout").await;
    let (status,community,_)=call(&app,Method::POST,"/v1/communities",
        Some(json!({"name":"Private Voice"})),Some(&owner)).await;
    assert_eq!(status,StatusCode::CREATED);
    let cid=community["id"].as_str().expect("community");
    let mkch=|name:&str,kind:&str|json!({"name":name,"channel_kind":kind,"position":0});
    let churl=format!("/v1/communities/{cid}/channels");
    let (status,voice,_)=call(&app,Method::POST,&churl,
        Some(mkch("party-voice","voice")),Some(&owner)).await;
    assert_eq!(status,StatusCode::CREATED);
    let voice_id=voice["id"].as_str().expect("voice id");
    let (status,text,_)=call(&app,Method::POST,&churl,
        Some(mkch("general","text")),Some(&owner)).await;
    assert_eq!(status,StatusCode::CREATED);
    let text_id=text["id"].as_str().expect("text id");
    let voice_path=format!("/v1/communities/{cid}/channels/{voice_id}/livekit-token");
    let text_path=format!("/v1/communities/{cid}/channels/{text_id}/livekit-token");

    let (code,_,_)=call(&app,Method::POST,&voice_path,None,None).await;
    assert_eq!(code,StatusCode::UNAUTHORIZED);
    let (code,_,_)=call(&app,Method::POST,&voice_path,None,Some(&outsider)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);
    let (code,_,_)=call(&app,Method::POST,&text_path,None,Some(&owner)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);
    let (code,_,_)=call(&app,Method::POST,
        &format!("/v1/communities/{cid}/channels/{text_id}/livekit-token"),
        None,Some(&member)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);

    let (code,token,no_store)=call(&app,Method::POST,&voice_path,None,Some(&owner)).await;
    assert_eq!(code,StatusCode::CREATED);
    assert!(no_store);
    assert_eq!(token["server_url"],"wss://voice.example.test");
    assert_eq!(token["room"],format!("gcp-voice-{voice_id}"));
    assert_eq!(token["expires_in"],300);
    let jwt=token["participant_token"].as_str().expect("JWT");
    assert_eq!(jwt.split('.').count(),3);
    assert!(!jwt.contains("test-secret"));

    let (code,invitation,_)=call(&app,Method::POST,
        &format!("/v1/communities/{cid}/invitations"),None,Some(&owner)).await;
    assert_eq!(code,StatusCode::CREATED);
    let (code,_,_)=call(&app,Method::POST,"/v1/invitations/accept",
        Some(json!({"token":invitation["token"]})),Some(&member)).await;
    assert_eq!(code,StatusCode::CREATED);
    let (code,member_token,_)=call(&app,Method::POST,&voice_path,None,Some(&member)).await;
    assert_eq!(code,StatusCode::CREATED);
    assert_ne!(member_token["participant_token"],token["participant_token"]);

    let (code,_,_)=call(&app,Method::POST,
        &format!("/v1/communities/{cid}/channels/{voice_id}/livekit-token"),
        None,Some(&outsider)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);
    let remove=format!("/v1/communities/{cid}/members/{member_id}");
    let (code,_,_)=call(&app,Method::DELETE,&remove,None,Some(&owner)).await;
    assert_eq!(code,StatusCode::NO_CONTENT);
    let (code,_,_)=call(&app,Method::POST,&voice_path,None,Some(&member)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);
    let (code,_,_)=call(&app,Method::POST,
        &format!("/v1/communities/{outsider_id}/channels/{voice_id}/livekit-token"),
        None,Some(&owner)).await;
    assert_eq!(code,StatusCode::NOT_FOUND);

    let (code,_,_)=call(&app,Method::POST,"/v1/auth/logout",
        None,Some(&owner)).await;
    assert_eq!(code,StatusCode::NO_CONTENT);
    let (code,_,_)=call(&app,Method::POST,&voice_path,None,Some(&owner)).await;
    assert_eq!(code,StatusCode::UNAUTHORIZED);

    // Test fixtures are isolated from concurrent integration tests.
    sqlx::query("DELETE FROM communities WHERE id=$1")
        .bind(Uuid::parse_str(cid).expect("UUID")).execute(&db).await.expect("cleanup");
    for id in [owner_id,member_id,outsider_id] {
        sqlx::query("DELETE FROM users WHERE id=$1")
            .bind(Uuid::parse_str(&id).expect("UUID"))
            .execute(&db).await.expect("cleanup user");
    }
}

#[tokio::test]
async fn missing_database_or_livekit_config_never_emits_tokens() {
    let fake="00000000-0000-0000-0000-000000000001";
    let uri=format!("/v1/communities/{fake}/channels/{fake}/livekit-token");
    let app=router_with_livekit(None,None);
    let (status,_,_)=call(&app,Method::POST,&uri,None,None).await;
    assert_eq!(status,StatusCode::SERVICE_UNAVAILABLE);
}
