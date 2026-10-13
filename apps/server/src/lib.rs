pub mod database;

use axum::{Json, Router, extract::State, http::StatusCode, routing::get};
use serde::Serialize;
use sqlx::PgPool;

#[derive(Serialize)]
struct HealthResponse {
    service: &'static str,
    status: &'static str,
}

#[derive(Clone, Default)]
struct AppState {
    database: Option<PgPool>,
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

/// Public HTTP routes. Authenticated business endpoints have not been
/// implemented; do not expose users, channels, or messages before auth.
pub fn router() -> Router {
    router_with_database(None)
}

pub fn router_with_database(database: Option<PgPool>) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/ready", get(ready))
        .with_state(AppState { database })
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
