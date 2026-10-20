//! Server-owned reconciliation of active GCP voice rooms. No client cooperation,
//! volatile event queue, or mutation hook is required: restart recovers from the
//! SFU and database. This namespace must be exclusive to one GCP deployment.
use std::{
    collections::HashSet,
    sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use crate::livekit_tokens::{LiveKitConfig, participant_identity, room_name};
use serde::{Deserialize, de::DeserializeOwned};
use serde_json::{Value, json};
use sqlx::{PgPool, types::Uuid};

const MAX_RESPONSE_BYTES: usize = 256 * 1024;
const MAX_ROOMS: usize = 64;
const MAX_PARTICIPANTS: usize = 256;
const CHECK_INTERVAL: Duration = Duration::from_secs(5);
const HEALTH_MAX_AGE_MS: u64 = 30_000;

#[derive(Debug)]
pub enum EvictionError {
    Transport,
    InvalidResponse,
    Capacity,
    Database,
    Credentials,
    Deadline,
}
impl std::fmt::Display for EvictionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("LiveKit access reconciliation unavailable")
    }
}
impl std::error::Error for EvictionError {}

pub(crate) struct EvictionHealth {
    started: Instant,
    success: AtomicU64,
    failed: AtomicBool,
    cursor: AtomicUsize,
}
impl EvictionHealth {
    pub(crate) fn new() -> Self {
        Self {
            started: Instant::now(),
            success: AtomicU64::new(0),
            failed: AtomicBool::new(true),
            cursor: AtomicUsize::new(0),
        }
    }
    fn mark(&self, success: bool) {
        if success {
            self.success.store(
                self.started.elapsed().as_millis() as u64 + 1,
                Ordering::Release,
            );
        }
        self.failed.store(!success, Ordering::Release);
    }
    fn healthy(&self) -> bool {
        let last = self.success.load(Ordering::Acquire);
        last != 0
            && !self.failed.load(Ordering::Acquire)
            && (self.started.elapsed().as_millis() as u64 + 1).saturating_sub(last)
                < HEALTH_MAX_AGE_MS
    }
}

#[derive(Default, Debug)]
pub struct EvictionReport {
    pub rooms_checked: usize,
    pub participants_checked: usize,
    pub removed: usize,
}
#[derive(Deserialize)]
struct Room {
    name: String,
}
#[derive(Deserialize)]
struct Participant {
    identity: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Rooms {
    #[serde(default)]
    rooms: Vec<Room>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Participants {
    #[serde(default)]
    participants: Vec<Participant>,
}

fn unix_seconds() -> Result<u64, EvictionError> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .map_err(|_| EvictionError::Credentials)
}
fn channel_id(name: &str) -> Option<Uuid> {
    let id = Uuid::parse_str(name.strip_prefix("gcp-voice-")?).ok()?;
    (room_name(id) == name).then_some(id)
}
fn session_id(identity: &str) -> Option<Uuid> {
    let id = Uuid::parse_str(identity.strip_prefix("gcp-session-")?).ok()?;
    (participant_identity(id) == identity).then_some(id)
}

impl LiveKitConfig {
    pub(crate) fn eviction_healthy(&self) -> bool {
        self.eviction_guard.healthy()
    }

    async fn rpc<T: DeserializeOwned>(
        &self,
        method: &str,
        room: Option<&str>,
        body: Value,
    ) -> Result<T, EvictionError> {
        let now = unix_seconds()?;
        let video = match room {
            Some(room) => json!({"roomAdmin": true, "room": room}),
            None => json!({"roomList": true}),
        };
        let token = self
            .sign_claims(json!({
                "iss": self.api_key, "iat": now, "nbf": now.saturating_sub(2),
                "exp": now + 30, "video": video,
            }))
            .map_err(|_| EvictionError::Credentials)?;
        let mut url = self.control_url.clone();
        url.set_path(&format!("/twirp/livekit.RoomService/{method}"));
        let mut response = self
            .http
            .post(url)
            .bearer_auth(token)
            .json(&body)
            .send()
            .await
            .map_err(|_| EvictionError::Transport)?;
        let status = response.status();
        if response
            .content_length()
            .is_some_and(|size| size > MAX_RESPONSE_BYTES as u64)
        {
            return Err(EvictionError::Capacity);
        }
        let mut bytes = Vec::new();
        while let Some(chunk) = response
            .chunk()
            .await
            .map_err(|_| EvictionError::Transport)?
        {
            if bytes.len() + chunk.len() > MAX_RESPONSE_BYTES {
                return Err(EvictionError::Capacity);
            }
            bytes.extend_from_slice(&chunk);
        }
        if method == "RemoveParticipant" && status == reqwest::StatusCode::NOT_FOUND {
            // A concurrent leave/removal is already the desired outcome. Do not
            // mistake an HTML proxy 404 or unrelated error for an SFU response.
            let error: Value =
                serde_json::from_slice(&bytes).map_err(|_| EvictionError::InvalidResponse)?;
            if error.get("code").and_then(Value::as_str) == Some("not_found") {
                return serde_json::from_value(json!({}))
                    .map_err(|_| EvictionError::InvalidResponse);
            }
        }
        if !status.is_success() {
            return Err(EvictionError::Transport);
        }
        serde_json::from_slice(&bytes).map_err(|_| EvictionError::InvalidResponse)
    }

    /// One bounded snapshot pass. No media operations run within DB locks.
    pub async fn reconcile_voice_access(
        &self,
        pool: &PgPool,
    ) -> Result<EvictionReport, EvictionError> {
        let result = tokio::time::timeout(Duration::from_secs(20), self.reconcile(pool))
            .await
            .unwrap_or(Err(EvictionError::Deadline));
        self.eviction_guard.mark(result.is_ok());
        result
    }

    async fn reconcile(&self, pool: &PgPool) -> Result<EvictionReport, EvictionError> {
        let mut rooms: Rooms = self.rpc("ListRooms", None, json!({})).await?;
        if rooms.rooms.len() > MAX_ROOMS {
            return Err(EvictionError::Capacity);
        }
        // Rotate deterministic room ordering across incomplete passes so one
        // slow/failing room cannot permanently starve all later rooms.
        rooms.rooms.sort_by(|a, b| a.name.cmp(&b.name));
        let count = rooms.rooms.len();
        if count > 0 {
            let start = self.eviction_guard.cursor.fetch_add(1, Ordering::Relaxed) % count;
            rooms.rooms.rotate_left(start);
        }
        let mut report = EvictionReport::default();
        let mut failure = None;
        for room in rooms.rooms {
            let Some(channel) = channel_id(&room.name) else {
                continue;
            };
            report.rooms_checked += 1;
            let participants: Participants = match self
                .rpc(
                    "ListParticipants",
                    Some(&room.name),
                    json!({"room":room.name}),
                )
                .await
            {
                Ok(value) => value,
                Err(error) => {
                    failure = Some(error);
                    continue;
                }
            };
            if participants.participants.len() > MAX_PARTICIPANTS {
                failure = Some(EvictionError::Capacity);
                continue;
            }
            let sessions: Vec<Uuid> = participants
                .participants
                .iter()
                .filter_map(|p| session_id(&p.identity))
                .collect();
            let allowed: HashSet<Uuid> = if sessions.is_empty() {
                HashSet::new()
            } else {
                sqlx::query_scalar::<_, Uuid>(
                    "SELECT s.id FROM user_sessions s
                     JOIN community_memberships m ON m.user_id=s.user_id
                     JOIN channels c ON c.community_id=m.community_id
                     WHERE s.id=ANY($1) AND c.id=$2 AND c.channel_kind='voice'
                       AND s.revoked_at IS NULL AND s.access_expires_at > now()
                       AND s.refresh_expires_at > now()",
                )
                .bind(sessions)
                .bind(channel)
                .fetch_all(pool)
                .await
                .map_err(|_| EvictionError::Database)?
                .into_iter()
                .collect()
            };
            for participant in participants.participants {
                report.participants_checked += 1;
                if participant.identity.is_empty() || participant.identity.len() > 512 {
                    failure = Some(EvictionError::InvalidResponse);
                    continue;
                }
                let authorized =
                    session_id(&participant.identity).is_some_and(|id| allowed.contains(&id));
                if !authorized {
                    let result: Result<Value, EvictionError> = self
                        .rpc(
                            "RemoveParticipant",
                            Some(&room.name),
                            json!({
                                "room":room.name, "identity":participant.identity,
                                // Explicit cutoff avoids Cloud's default one-minute buffer.
                                "revoke_token_ts": unix_seconds()? + 1,
                            }),
                        )
                        .await;
                    match result {
                        Ok(_) => report.removed += 1,
                        Err(error) => failure = Some(error),
                    }
                }
            }
        }
        match failure {
            Some(error) => Err(error),
            None => Ok(report),
        }
    }

    /// Reconcile actual room state after restarts, retries and external DB edits.
    /// Errors contain no URLs, tokens or response bodies; retry on the next tick.
    pub async fn run_eviction_loop(&self, pool: &PgPool) {
        let mut tick = tokio::time::interval(CHECK_INTERVAL);
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tick.tick().await;
            if self.reconcile_voice_access(pool).await.is_err() {
                tracing::warn!(
                    "LiveKit access reconciliation failed; new voice grants are unavailable"
                );
            }
        }
    }
}

#[cfg(test)]
#[path = "livekit_control_contract.rs"]
mod control_contract;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn namespace_is_strict_and_health_starts_unready() {
        let id = Uuid::from_u128(7);
        assert_eq!(channel_id(&room_name(id)), Some(id));
        assert_eq!(session_id(&participant_identity(id)), Some(id));
        assert_eq!(channel_id("unrelated-room"), None);
        assert_eq!(session_id(&id.to_string()), None);
        assert_eq!(session_id("gcp-session-garbage"), None);
        let health = EvictionHealth::new();
        assert!(!health.healthy());
        health.mark(true);
        assert!(health.healthy());
        health.mark(false);
        assert!(!health.healthy());
    }
}
