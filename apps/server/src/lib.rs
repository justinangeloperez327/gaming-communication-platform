use axum::{Json, Router, routing::get};
use serde::Serialize;

#[derive(Serialize)]
struct HealthResponse {
    service: &'static str,
    status: &'static str,
}

async fn health() -> Json<HealthResponse> {
    Json(HealthResponse {
        service: "gaming-comms-server",
        status: "ok",
    })
}

/// Public HTTP routes. Business routes will be added only after authentication
/// and authorization boundaries are in place.
pub fn router() -> Router {
    Router::new().route("/health", get(health))
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
