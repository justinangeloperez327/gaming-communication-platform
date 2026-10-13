//! Single-process authenticated channel event sockets.
//! Transport events contain identifiers only (no stored message bodies).
//! Every forwarded event rechecks the caller's current DB membership/session.
//! No cross-instance fanout or durable replay: HTTP history is authoritative.

use std::{collections::HashMap, sync::Arc, time::Duration};

use axum::{
    extract::{
        Path, State,
        ws::{CloseFrame, Message, WebSocket, WebSocketUpgrade, close_code},
    },
    http::HeaderMap,
    response::IntoResponse,
};
use serde::{Deserialize, Serialize};
use sqlx::{PgPool, types::Uuid};
use tokio::{
    sync::{Mutex, broadcast},
    time::{self, Instant},
};

use crate::{
    AppState,
    auth::{self, AuthError},
    messages,
};

const EVENT_QUEUE_CAPACITY: usize = 256;
const MAX_CONNECTIONS_PER_USER_CHANNEL: usize = 4;
const MAX_CHANNEL_CONNECTIONS: usize = 200;
const OUTGOING_TIMEOUT: Duration = Duration::from_secs(3);
const AUTH_CHECK: Duration = Duration::from_secs(10);
const IDLE_TIMEOUT: Duration = Duration::from_secs(75);
const TYPING_THROTTLE: Duration = Duration::from_secs(2);

#[derive(Clone, Copy, Debug)]
pub(crate) enum EventKind {
    Created,
    Edited,
    Deleted,
    Typing,
    Joined,
    Left,
}

impl EventKind {
    fn name(self) -> &'static str {
        match self {
            Self::Created => "message_created",
            Self::Edited => "message_edited",
            Self::Deleted => "message_deleted",
            Self::Typing => "typing",
            Self::Joined => "presence_joined",
            Self::Left => "presence_left",
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct Event {
    pub channel: Uuid,
    actor: Uuid,
    message: Option<Uuid>,
    kind: EventKind,
    active: Option<bool>,
}

impl Event {
    pub fn message(channel: Uuid, actor: Uuid, id: Uuid, kind: EventKind) -> Self {
        Self {
            channel,
            actor,
            message: Some(id),
            kind,
            active: None,
        }
    }

    fn presence(channel: Uuid, actor: Uuid, kind: EventKind) -> Self {
        Self {
            channel,
            actor,
            message: None,
            kind,
            active: None,
        }
    }

    fn typing(channel: Uuid, actor: Uuid, active: bool) -> Self {
        Self {
            channel,
            actor,
            message: None,
            kind: EventKind::Typing,
            active: Some(active),
        }
    }
}

#[derive(Clone, Default)]
pub(crate) struct EventHub {
    bus: Option<broadcast::Sender<Event>>,
    presence: Arc<Mutex<HashMap<(Uuid, Uuid), usize>>>,
}

impl EventHub {
    pub fn new() -> Self {
        let (bus, _) = broadcast::channel(EVENT_QUEUE_CAPACITY);
        Self {
            bus: Some(bus),
            presence: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    pub fn publish(&self, event: Event) {
        // No receiver is not an error. HTTP storage succeeded independently.
        if let Some(bus) = &self.bus {
            let _ = bus.send(event);
        }
    }

    fn listen(&self) -> Option<broadcast::Receiver<Event>> {
        self.bus.as_ref().map(broadcast::Sender::subscribe)
    }

    async fn join(&self, channel: Uuid, actor: Uuid) -> bool {
        let mut presence = self.presence.lock().await;
        let user_count = presence.get(&(channel, actor)).copied().unwrap_or(0);
        let channel_count: usize = presence
            .iter()
            .filter(|((id, _), _)| *id == channel)
            .map(|(_, n)| *n)
            .sum();
        if user_count >= MAX_CONNECTIONS_PER_USER_CHANNEL
            || channel_count >= MAX_CHANNEL_CONNECTIONS
        {
            return false;
        }
        presence.insert((channel, actor), user_count + 1);
        let first = user_count == 0;
        drop(presence);
        if first {
            self.publish(Event::presence(channel, actor, EventKind::Joined));
        }
        true
    }

    async fn leave(&self, channel: Uuid, actor: Uuid) {
        let mut presence = self.presence.lock().await;
        match presence.get(&(channel, actor)).copied() {
            Some(1) => {
                presence.remove(&(channel, actor));
                drop(presence);
                self.publish(Event::presence(channel, actor, EventKind::Left));
            }
            Some(n) => {
                presence.insert((channel, actor), n - 1);
            }
            None => {}
        }
    }
}

#[derive(Deserialize)]
struct ClientCommand {
    #[serde(rename = "type")]
    kind: String,
    active: Option<bool>,
}

#[derive(Serialize)]
struct ServerEvent<'a> {
    #[serde(rename = "type")]
    kind: &'a str,
    channel_id: String,
    user_id: Option<String>,
    message_id: Option<String>,
    active: Option<bool>,
}

fn encode(event: &Event) -> Result<String, serde_json::Error> {
    serde_json::to_string(&ServerEvent {
        kind: event.kind.name(),
        channel_id: event.channel.to_string(),
        user_id: Some(event.actor.to_string()),
        message_id: event.message.map(|id| id.to_string()),
        active: event.active,
    })
}

async fn send_text(socket: &mut WebSocket, payload: String) -> bool {
    matches!(
        time::timeout(OUTGOING_TIMEOUT, socket.send(Message::Text(payload.into()))).await,
        Ok(Ok(()))
    )
}

async fn valid_subscriber(
    pool: &PgPool,
    headers: &HeaderMap,
    community: Uuid,
    channel: Uuid,
    user: Uuid,
) -> bool {
    let Ok((current, _)) = auth::authenticated_user(pool, headers).await else {
        return false;
    };
    current == user
        && messages::text_channel(pool, community, channel, user)
            .await
            .is_ok()
}

pub(crate) async fn subscribe(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((community_id, channel_id)): Path<(String, String)>,
    upgrade: WebSocketUpgrade,
) -> Result<impl IntoResponse, AuthError> {
    // The Qt/native client must supply Authorization during the handshake.
    // Raw access tokens are never permitted in URL query strings.
    let community = Uuid::parse_str(&community_id).map_err(|_| AuthError::InvalidRequest)?;
    let channel = Uuid::parse_str(&channel_id).map_err(|_| AuthError::InvalidRequest)?;
    let pool = auth::database(&state)?;
    let (user, _) = auth::authenticated_user(pool, &headers).await?;
    messages::text_channel(pool, community, channel, user).await?;
    let pool = pool.clone();
    let hub = state.realtime.clone();
    Ok(upgrade
        .max_message_size(2048)
        .max_frame_size(2048)
        .on_upgrade(move |socket| handle(socket, hub, pool, headers, community, channel, user)))
}

async fn handle(
    mut socket: WebSocket,
    hub: EventHub,
    pool: PgPool,
    headers: HeaderMap,
    community: Uuid,
    channel: Uuid,
    user: Uuid,
) {
    // Subscribe before registration, so the channel_join event is observable.
    let Some(mut listener) = hub.listen() else {
        return;
    };
    if !hub.join(channel, user).await {
        let _ = socket
            .send(Message::Close(Some(CloseFrame {
                code: close_code::POLICY,
                reason: "connection_limit".into(),
            })))
            .await;
        return;
    }
    let ready = serde_json::json!({
        "type": "ready", "channel_id": channel.to_string(),
        "recovery": "GET /v1/communities/{community_id}/channels/{channel_id}/messages"
    });
    if !send_text(&mut socket, ready.to_string()).await {
        hub.leave(channel, user).await;
        return;
    }

    let mut periodic = time::interval(AUTH_CHECK);
    periodic.set_missed_tick_behavior(time::MissedTickBehavior::Delay);
    let mut last_inbound = Instant::now();
    let mut last_typing = Instant::now() - TYPING_THROTTLE;
    loop {
        tokio::select! {
            _ = periodic.tick() => {
                if last_inbound.elapsed() >= IDLE_TIMEOUT
                    || !valid_subscriber(&pool, &headers, community, channel, user).await {
                    break;
                }
                if time::timeout(OUTGOING_TIMEOUT,
                    socket.send(Message::Ping(Vec::new().into()))).await
                    .map_or(true, |out| out.is_err())
                {
                    break;
                }
            }
            incoming = socket.recv() => match incoming {
                Some(Ok(Message::Text(input))) => {
                    last_inbound = Instant::now();
                    // Only bounded ephemeral typing notifications are accepted.
                    // Message writes still go through authenticated HTTP CRUD.
                    if let Ok(cmd) = serde_json::from_str::<ClientCommand>(&input) {
                        if cmd.kind == "typing"
                            && let Some(active) = cmd.active
                            && last_typing.elapsed() >= TYPING_THROTTLE
                        {
                            if !valid_subscriber(&pool, &headers, community, channel, user).await {
                                break;
                            }
                            hub.publish(Event::typing(channel, user, active));
                            last_typing = Instant::now();
                        }
                    }
                }
                Some(Ok(Message::Ping(_) | Message::Pong(_))) => {
                    last_inbound = Instant::now();
                }
                Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                Some(Ok(Message::Binary(_))) => break,
            },
            event = listener.recv() => match event {
                Ok(event) if event.channel == channel => {
                    // Re-authorize for EACH outbound event so a removed user or
                    // revoked session cannot continue observing activity.
                    if !valid_subscriber(&pool, &headers, community, channel, user).await {
                        break;
                    }
                    match encode(&event) {
                        Ok(payload) if send_text(&mut socket, payload).await => {}
                        _ => break,
                    }
                }
                Ok(_) => {}
                Err(broadcast::error::RecvError::Lagged(_)) => {
                    // Explicitly signal a gap; client MUST fetch HTTP history.
                    let _ = send_text(&mut socket,
                        serde_json::json!({"type":"resync_required"}).to_string()).await;
                    break;
                }
                Err(broadcast::error::RecvError::Closed) => break,
            },
        }
    }
    hub.leave(channel, user).await;
    let _ = time::timeout(OUTGOING_TIMEOUT, socket.send(Message::Close(None))).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn hub_tracks_unique_presence_and_enforces_bounded_connections() {
        let hub = EventHub::new();
        let channel = Uuid::from_u128(1);
        let user = Uuid::from_u128(2);
        let mut observer = hub.listen().expect("sender");
        for _ in 0..MAX_CONNECTIONS_PER_USER_CHANNEL {
            assert!(hub.join(channel, user).await);
        }
        let joined = observer.try_recv().expect("join");
        assert!(matches!(joined.kind, EventKind::Joined));
        assert!(observer.try_recv().is_err());
        assert!(!hub.join(channel, user).await);
        for _ in 0..MAX_CONNECTIONS_PER_USER_CHANNEL {
            hub.leave(channel, user).await;
        }
        let left = observer.try_recv().expect("leave");
        assert!(matches!(left.kind, EventKind::Left));
    }

    #[test]
    fn identifier_only_message_events() {
        let ev = Event::message(
            Uuid::from_u128(3),
            Uuid::from_u128(4),
            Uuid::from_u128(5),
            EventKind::Created,
        );
        let encoded = encode(&ev).expect("JSON");
        assert!(encoded.contains("message_created"));
        assert!(!encoded.contains("body"));
        assert!(!encoded.contains("token"));
    }
}
