pub mod auth;
mod communities;
pub mod database;
mod direct_messages;
mod friends;
pub mod livekit_tokens;
mod messages;
mod realtime;

use axum::{
    Json, Router,
    extract::{DefaultBodyLimit, State},
    http::StatusCode,
    routing::{get, post},
};
use serde::Serialize;
use sqlx::PgPool;

#[derive(Serialize)]
struct HealthResponse {
    service: &'static str,
    status: &'static str,
}

#[derive(Clone, Default)]
pub(crate) struct AppState {
    pub(crate) database: Option<PgPool>,
    pub(crate) livekit: Option<livekit_tokens::LiveKitConfig>,
    pub(crate) realtime: realtime::EventHub,
}

async fn health() -> Json<HealthResponse> {
    Json(HealthResponse {
        service: "gaming-comms-server",
        status: "ok",
    })
}

/// Unlike liveness, readiness requires an actual working PostgreSQL pool.
/// Never disclose connection errors or credentials to HTTP clients.
async fn ready(State(state): State<AppState>) -> StatusCode {
    let Some(pool) = &state.database else {
        return StatusCode::SERVICE_UNAVAILABLE;
    };
    match sqlx::query_scalar::<_, i32>("SELECT 1")
        .fetch_one(pool)
        .await
    {
        Ok(1) => StatusCode::NO_CONTENT,
        _ => StatusCode::SERVICE_UNAVAILABLE,
    }
}

async fn no_store(mut response: axum::response::Response) -> axum::response::Response {
    response.headers_mut().insert(
        axum::http::header::CACHE_CONTROL,
        axum::http::HeaderValue::from_static("no-store"),
    );
    response
}

/// Public health endpoints and database-authorized business routes.
pub fn router() -> Router {
    router_with_database(None)
}

pub fn router_with_database(database: Option<PgPool>) -> Router {
    router_with_livekit(database, None)
}

/// Optional LiveKit token issuance is enabled only when credentials are
/// configured on the server. No API secrets are exposed to the desktop.
pub fn router_with_livekit(
    database: Option<PgPool>,
    livekit: Option<livekit_tokens::LiveKitConfig>,
) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/ready", get(ready))
        .route("/v1/auth/register", post(auth::register))
        .route("/v1/auth/login", post(auth::login))
        .route("/v1/auth/refresh", post(auth::refresh))
        .route("/v1/auth/logout", post(auth::logout))
        .route("/v1/me", get(auth::me).patch(auth::update_profile))
        .route(
            "/v1/communities",
            get(communities::list).post(communities::create),
        )
        .route(
            "/v1/communities/{community_id}",
            get(communities::get)
                .patch(communities::rename)
                .delete(communities::delete),
        )
        .route(
            "/v1/communities/{community_id}/members",
            get(communities::members),
        )
        .route(
            "/v1/communities/{community_id}/members/{member_id}",
            axum::routing::patch(communities::change_role).delete(communities::remove_member),
        )
        .route(
            "/v1/communities/{community_id}/invitations",
            post(communities::invite),
        )
        .route("/v1/invitations/accept", post(communities::accept_invite))
        .route(
            "/v1/communities/{community_id}/channels",
            get(communities::channels).post(communities::create_channel),
        )
        .route(
            "/v1/communities/{community_id}/channels/{channel_id}",
            axum::routing::patch(communities::update_channel).delete(communities::delete_channel),
        )
        .route(
            "/v1/communities/{community_id}/channels/{channel_id}/messages",
            get(messages::list).post(messages::create),
        )
        .route(
            "/v1/communities/{community_id}/channels/{channel_id}/messages/{message_id}",
            get(messages::get)
                .patch(messages::edit)
                .delete(messages::delete),
        )
        .route(
            "/v1/communities/{community_id}/channels/{channel_id}/events",
            get(realtime::subscribe),
        )
        .route(
            "/v1/communities/{community_id}/channels/{channel_id}/livekit-token",
            post(livekit_tokens::issue),
        )
        .route("/v1/friends", get(friends::list_friends))
        .route(
            "/v1/friends/{user_id}",
            axum::routing::delete(friends::remove_friend),
        )
        .route(
            "/v1/friends/requests",
            get(friends::requests).post(friends::send_request),
        )
        .route(
            "/v1/friends/requests/{request_id}/accept",
            post(friends::accept),
        )
        .route(
            "/v1/friends/requests/{request_id}/decline",
            post(friends::decline),
        )
        .route("/v1/blocks", get(friends::blocks).post(friends::block))
        .route(
            "/v1/blocks/{user_id}",
            axum::routing::delete(friends::unblock),
        )
        .route(
            "/v1/dms",
            get(direct_messages::list).post(direct_messages::open),
        )
        .route(
            "/v1/dms/{conversation_id}/messages",
            get(direct_messages::history).post(direct_messages::send),
        )
        .route(
            "/v1/dms/{conversation_id}/messages/{message_id}",
            axum::routing::patch(direct_messages::edit).delete(direct_messages::delete),
        )
        // Session credentials and private API responses must not be cached by
        // browsers or reverse proxies, including authentication error replies.
        .layer(axum::middleware::map_response(no_store))
        .layer(DefaultBodyLimit::max(8 * 1024))
        .with_state(AppState {
            database,
            livekit,
            realtime: realtime::EventHub::new(),
        })
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{
        body::{Body, to_bytes},
        http::{Request, StatusCode},
    };
    use tower::ServiceExt;

    #[tokio::test]
    async fn health_returns_json() {
        let response = router()
            .oneshot(
                Request::builder()
                    .uri("/health")
                    .body(Body::empty())
                    .expect("valid request"),
            )
            .await
            .expect("infallible router");

        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()["cache-control"], "no-store");

        let body = to_bytes(response.into_body(), 1024)
            .await
            .expect("response body");
        let json: serde_json::Value = serde_json::from_slice(&body).expect("valid JSON");
        assert_eq!(json["service"], "gaming-comms-server");
        assert_eq!(json["status"], "ok");
    }

    #[tokio::test]
    async fn ready_without_database_is_unavailable() {
        let response = router()
            .oneshot(
                Request::builder()
                    .uri("/ready")
                    .body(Body::empty())
                    .expect("valid request"),
            )
            .await
            .expect("infallible router");
        assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
    }

    #[tokio::test]
    async fn unspecified_route_is_not_found() {
        let response = router()
            .oneshot(
                Request::builder()
                    .uri("/v1/messages")
                    .body(Body::empty())
                    .expect("valid request"),
            )
            .await
            .expect("infallible router");

        assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }
}
