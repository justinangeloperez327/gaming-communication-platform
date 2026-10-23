//! Server-only LiveKit HS256 room credentials.
//!
//! Join grants expire after five minutes. This limits future join/reconnect
//! attempts, NOT an existing participant already connected to the LiveKit SFU.
//! Server-side reconciliation removes participants whose backing session or
//! channel access is no longer valid. Self-hosted JWT replay remains possible.

use axum::{
    Json,
    extract::{Path, State},
    http::{HeaderMap, StatusCode, header},
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use rand::{RngCore, rngs::OsRng};
use serde::Serialize;
use sha2::Sha256;
use sqlx::types::Uuid;
use std::{
    env, io,
    time::{SystemTime, UNIX_EPOCH},
};

use crate::{
    AppState,
    auth::{self, AuthError},
};

const JOIN_TTL_SECONDS: u64 = 300;
const CLOCK_SKEW_SECONDS: u64 = 2;
type HmacSha256 = Hmac<Sha256>;

#[derive(Clone)]
pub struct LiveKitConfig {
    server_url: String,
    pub(crate) api_key: String,
    pub(crate) api_secret: String,
    pub(crate) control_url: reqwest::Url,
    pub(crate) http: reqwest::Client,
    pub(crate) eviction_guard: std::sync::Arc<crate::livekit_eviction::EvictionHealth>,
}

impl LiveKitConfig {
    /// Invalid or partially present credentials fail server startup.
    /// No credential value is included in error messages or Debug output.
    pub fn from_env() -> Result<Option<Self>, io::Error> {
        let url = env::var("LIVEKIT_URL").ok();
        let key = env::var("LIVEKIT_API_KEY").ok();
        let secret = env::var("LIVEKIT_API_SECRET").ok();
        let control = env::var("LIVEKIT_CONTROL_URL").ok();
        match (url, key, secret, control) {
            (None, None, None, None) => Ok(None),
            (Some(url), Some(key), Some(secret), control) => {
                let config = Self::new(url, key, secret)?;
                match control {
                    Some(control) => config.with_control_url(&control).map(Some),
                    None => Ok(Some(config)),
                }
            }
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "LiveKit credentials must be configured together",
            )),
        }
    }

    // An explicit server-only origin avoids routing internal control traffic
    // through public DNS/TLS. Plain HTTP remains restricted to literal loopback.
    fn with_control_url(mut self, raw: &str) -> Result<Self, io::Error> {
        let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "Invalid LiveKit control URL");
        let url = reqwest::Url::parse(raw).map_err(|_| invalid())?;
        let secure = url.scheme() == "https" && url.host_str().is_some();
        let local =
            url.scheme() == "http" && matches!(url.host_str(), Some("127.0.0.1" | "localhost"));
        if (!secure && !local)
            || raw.len() > 2048
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
            || url.path() != "/"
            || raw.bytes().any(|b| b.is_ascii_whitespace())
            || raw.contains(['?', '#', '@'])
        {
            return Err(invalid());
        }
        self.control_url = url;
        Ok(self)
    }

    pub fn new(server_url: String, api_key: String, api_secret: String) -> Result<Self, io::Error> {
        // Parse the authority, not just a prefix: cleartext is loopback-only.
        let url = reqwest::Url::parse(&server_url).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "Invalid LiveKit URL configuration",
            )
        })?;
        let secure = url.scheme() == "wss" && url.host_str().is_some();
        let local =
            url.scheme() == "ws" && matches!(url.host_str(), Some("127.0.0.1" | "localhost"));
        if (!secure && !local)
            || server_url.len() > 2048
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
            || (url.path() != "/" && !url.path().is_empty())
            || server_url.bytes().any(|b| b.is_ascii_whitespace())
            || server_url.contains(['?', '#', '@'])
            || api_key.len() < 3
            || api_key.len() > 128
            || api_secret.len() < 32
            || api_secret.len() > 512
            || api_key.bytes().any(|b| b.is_ascii_whitespace())
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Invalid LiveKit URL or credential configuration",
            ));
        }
        let mut control_url = url;
        control_url
            .set_scheme(if secure { "https" } else { "http" })
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "Invalid LiveKit scheme"))?;
        let http = reqwest::Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .no_proxy()
            .connect_timeout(std::time::Duration::from_secs(2))
            .timeout(std::time::Duration::from_secs(3))
            .pool_max_idle_per_host(2)
            .build()
            .map_err(|_| io::Error::other("Cannot initialize LiveKit HTTP client"))?;
        Ok(Self {
            server_url,
            api_key,
            api_secret,
            control_url,
            http,
            eviction_guard: std::sync::Arc::new(crate::livekit_eviction::EvictionHealth::new()),
        })
    }

    fn sign(&self, session: Uuid, channel: Uuid, now: u64) -> Result<String, AuthError> {
        self.sign_join(participant_identity(session), room_name(channel), &["microphone"], true,
            now, now.checked_add(JOIN_TTL_SECONDS).ok_or(AuthError::Unavailable)?)
    }

    fn sign_join(&self, identity: String, room: String, sources: &[&str], subscribe: bool,
        now: u64, expiry: u64) -> Result<String, AuthError> {
        let mut nonce = [0u8; 16];
        OsRng.fill_bytes(&mut nonce);
        let claims = serde_json::json!({
            "iss": self.api_key,
            "sub": identity,
            "iat": now,
            "nbf": now.saturating_sub(CLOCK_SKEW_SECONDS),
            "exp": expiry,
            "jti": hex(&nonce),
            "video": {
                "roomJoin": true,
                "room": room,
                "canPublish": !sources.is_empty(),
                "canPublishSources": sources,
                "canSubscribe": subscribe,
                "canPublishData": false,
                "canUpdateOwnMetadata": false,
            }
        });
        self.sign_claims(claims)
    }

    pub(crate) fn screen_token(&self, lease: Uuid, channel: Uuid,
        purpose: crate::screen_sessions::Purpose, expiry: u64) -> Result<TokenResponse, AuthError> {
        use crate::screen_sessions::{self, Purpose};
        let now = SystemTime::now().duration_since(UNIX_EPOCH)
            .map_err(|_| AuthError::Unavailable)?.as_secs();
        if !self.eviction_healthy() || expiry <= now || expiry > now + screen_sessions::LEASE_SECONDS {
            return Err(AuthError::Unavailable);
        }
        let room = screen_sessions::room_name(channel);
        let sources: &[&str] = match purpose { Purpose::Publish => &["screen_share"], Purpose::View => &[] };
        let participant_token = self.sign_join(screen_sessions::identity(lease, purpose), room.clone(),
            sources, purpose == Purpose::View, now, expiry)?;
        Ok(TokenResponse { server_url: self.server_url.clone(), participant_token, room,
            expires_in: expiry - now })
    }

    pub(crate) fn sign_claims(&self, claims: serde_json::Value) -> Result<String, AuthError> {
        let header = serde_json::json!({"alg": "HS256", "typ": "JWT"});
        let header = serde_json::to_vec(&header).map_err(|_| AuthError::Unavailable)?;
        let claims = serde_json::to_vec(&claims).map_err(|_| AuthError::Unavailable)?;
        let unsigned = format!(
            "{}.{}",
            URL_SAFE_NO_PAD.encode(header),
            URL_SAFE_NO_PAD.encode(claims)
        );
        let mut signer = HmacSha256::new_from_slice(self.api_secret.as_bytes())
            .map_err(|_| AuthError::Unavailable)?;
        signer.update(unsigned.as_bytes());
        let signature = signer.finalize().into_bytes();
        Ok(format!(
            "{}.{}",
            unsigned,
            URL_SAFE_NO_PAD.encode(signature)
        ))
    }
}

fn hex(input: &[u8]) -> String {
    const CHARSET: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(input.len() * 2);
    for b in input {
        result.push(CHARSET[usize::from(b >> 4)] as char);
        result.push(CHARSET[usize::from(b & 15)] as char);
    }
    result
}

pub(crate) fn participant_identity(session: Uuid) -> String {
    format!("gcp-session-{session}")
}

pub(crate) fn room_name(channel: Uuid) -> String {
    format!("gcp-voice-{channel}")
}

#[derive(Serialize)]
pub(crate) struct TokenResponse {
    server_url: String,
    participant_token: String,
    room: String,
    expires_in: u64,
}

pub(crate) async fn issue(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
) -> Result<
    (
        StatusCode,
        [(header::HeaderName, &'static str); 2],
        Json<TokenResponse>,
    ),
    AuthError,
> {
    let pool = auth::database(&state)?;
    let (user, session, _) = auth::authenticated_session(pool, &headers).await?;
    let community = Uuid::parse_str(&community_id).map_err(|_| AuthError::InvalidRequest)?;
    let channel = Uuid::parse_str(&channel_id).map_err(|_| AuthError::InvalidRequest)?;

    // Enforce current membership, correct community, and voice channel kind
    // IN THE DATABASE. Never mint credentials for arbitrary requested rooms.
    let authorized: bool = sqlx::query_scalar(
        "SELECT EXISTS (
            SELECT 1 FROM channels c
            JOIN community_memberships m ON m.community_id = c.community_id
            WHERE c.id = $1 AND c.community_id = $2
              AND c.channel_kind = 'voice' AND m.user_id = $3
         )",
    )
    .bind(channel)
    .bind(community)
    .bind(user)
    .fetch_one(pool)
    .await
    .map_err(|_| AuthError::Unavailable)?;
    if !authorized {
        return Err(AuthError::NotFound);
    }

    let config = state.livekit.as_ref().ok_or(AuthError::Unavailable)?;
    if !config.eviction_healthy() {
        return Err(AuthError::Unavailable);
    }
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| AuthError::Unavailable)?
        .as_secs();
    let token = config.sign(session, channel, now)?;
    Ok((
        StatusCode::CREATED,
        [
            (header::CACHE_CONTROL, "no-store"),
            (header::PRAGMA, "no-cache"),
        ],
        Json(TokenResponse {
            server_url: config.server_url.clone(),
            participant_token: token,
            room: room_name(channel),
            expires_in: JOIN_TTL_SECONDS,
        }),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> LiveKitConfig {
        LiveKitConfig::new(
            "wss://voice.example.test".into(),
            "test-livekit-key".into(),
            "an-example-livekit-secret-longer-than-32-bytes".into(),
        )
        .expect("valid test configuration")
    }

    #[test]
    fn control_override_keeps_public_wss_and_rejects_unsafe_origins() {
        let configured = config()
            .with_control_url("http://127.0.0.1:7880")
            .expect("loopback control");
        assert_eq!(configured.server_url, "wss://voice.example.test");
        assert_eq!(configured.control_url.as_str(), "http://127.0.0.1:7880/");
        assert!(
            config()
                .with_control_url("https://control.example.test")
                .is_ok()
        );
        for invalid in [
            "http://livekit:7880",
            "http://localhost.evil.test",
            "ws://127.0.0.1:7880",
            "https://user:secret@control.example.test",
            "https://control.example.test/path",
            "http://127.0.0.1:7880?x=1",
            "https://control.example.test/#fragment",
        ] {
            assert!(config().with_control_url(invalid).is_err());
        }
    }

    #[test]
    fn rejects_insecure_remote_urls_and_short_secrets() {
        assert!(
            LiveKitConfig::new(
                "ws://public.example.test".into(),
                "key".into(),
                "a".repeat(40)
            )
            .is_err()
        );
        assert!(
            LiveKitConfig::new(
                "wss://voice.example.test".into(),
                "key".into(),
                "short".into()
            )
            .is_err()
        );
        assert!(
            LiveKitConfig::new("ws://127.0.0.1:7880".into(), "key".into(), "a".repeat(40)).is_ok()
        );
    }

    #[test]
    fn jwt_has_restricted_livekit_grants_and_valid_hs256_signature() {
        let config = config();
        let user = Uuid::from_u128(1);
        let channel = Uuid::from_u128(2);
        let token = config.sign(user, channel, 1_780_000_000).expect("JWT");
        let parts: Vec<&str> = token.split('.').collect();
        assert_eq!(parts.len(), 3);
        let header: serde_json::Value =
            serde_json::from_slice(&URL_SAFE_NO_PAD.decode(parts[0]).expect("base64 header"))
                .expect("header JSON");
        assert_eq!(header["alg"], "HS256");
        let body: serde_json::Value =
            serde_json::from_slice(&URL_SAFE_NO_PAD.decode(parts[1]).expect("base64 claims"))
                .expect("claims JSON");
        assert_eq!(body["iss"], "test-livekit-key");
        assert_eq!(body["sub"], participant_identity(user));
        assert_eq!(body["video"]["room"], room_name(channel));
        assert_eq!(body["video"]["roomJoin"], true);
        assert_eq!(body["video"]["canPublish"], true);
        assert_eq!(
            body["video"]["canPublishSources"],
            serde_json::json!(["microphone"])
        );
        assert_eq!(body["video"]["canSubscribe"], true);
        assert_eq!(body["video"]["canPublishData"], false);
        assert!(body["video"].get("roomAdmin").is_none());
        assert!(body["video"].get("roomCreate").is_none());
        assert!(body["video"].get("roomRecord").is_none());
        assert_eq!(body["exp"].as_u64(), Some(1_780_000_300));
        let mut verifier =
            HmacSha256::new_from_slice(config.api_secret.as_bytes()).expect("valid key");
        verifier.update(format!("{}.{}", parts[0], parts[1]).as_bytes());
        verifier
            .verify_slice(&URL_SAFE_NO_PAD.decode(parts[2]).expect("signature"))
            .expect("valid HS256 HMAC");
        assert_ne!(
            config.sign(user, channel, 1_780_000_000).expect("second"),
            token
        );
        assert!(!token.contains(&config.api_secret));
    }
}
