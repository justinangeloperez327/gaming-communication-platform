//! Requires an explicit disposable GCP_TEST_DATABASE_URL.
//! CI provisions PostgreSQL; local runs without the variable skip safely.
use axum::{
    body::Body,
    http::{Request, StatusCode},
};
use gaming_comms_server::{database::MIGRATOR, router_with_database};
use sqlx::{PgPool, Row, postgres::PgPoolOptions, types::Uuid};
use tower::ServiceExt;

async fn connect_test_db() -> Option<PgPool> {
    let url = match std::env::var("GCP_TEST_DATABASE_URL") {
        Ok(url) => url,
        Err(std::env::VarError::NotPresent) => return None,
        Err(_) => panic!("Invalid GCP_TEST_DATABASE_URL Unicode"),
    };
    Some(
        PgPoolOptions::new()
            .max_connections(3)
            .connect(&url)
            .await
            .expect("disposable PostgreSQL test database is reachable"),
    )
}

#[tokio::test]
async fn postgres_schema_roundtrip_and_readiness() {
    let Some(pool) = connect_test_db().await else {
        return;
    };

    MIGRATOR.run(&pool).await.expect("run database migrations");
    // Idempotent even when multiple server replicas start at once, but
    // production operators should use a dedicated migration job.
    MIGRATOR
        .run(&pool)
        .await
        .expect("migrations are idempotent");

    let response = router_with_database(Some(pool.clone()))
        .oneshot(
            Request::builder()
                .uri("/ready")
                .body(Body::empty())
                .expect("request"),
        )
        .await
        .expect("router");
    assert_eq!(response.status(), StatusCode::NO_CONTENT);

    let mut transaction = pool.begin().await.expect("begin transaction");
    let user_id: Uuid = sqlx::query_scalar(
        "INSERT INTO users (handle, display_name, email, password_hash)
         VALUES ($1, $2, $3, $4) RETURNING id",
    )
    .bind("first_user")
    .bind("First User")
    .bind("first_user@example.test")
    .bind("$argon2id$v=19$m=65536,t=3,p=1$placeholder$placeholder")
    .fetch_one(&mut *transaction)
    .await
    .expect("create temporary test user");

    let community_id: Uuid = sqlx::query_scalar(
        "INSERT INTO communities (name, owner_id) VALUES ($1, $2)
         RETURNING id",
    )
    .bind("Test Community")
    .bind(user_id)
    .fetch_one(&mut *transaction)
    .await
    .expect("create community");

    sqlx::query(
        "INSERT INTO community_memberships (community_id, user_id, role)
         VALUES ($1, $2, 'owner')",
    )
    .bind(community_id)
    .bind(user_id)
    .execute(&mut *transaction)
    .await
    .expect("create membership");

    let channel_id: Uuid = sqlx::query_scalar(
        "INSERT INTO channels (community_id, name, channel_kind)
         VALUES ($1, $2, 'text') RETURNING id",
    )
    .bind(community_id)
    .bind("general")
    .fetch_one(&mut *transaction)
    .await
    .expect("create text channel");

    sqlx::query("INSERT INTO messages (channel_id, author_id, body) VALUES ($1, $2, $3)")
        .bind(channel_id)
        .bind(user_id)
        .bind("Hello from migration smoke test")
        .execute(&mut *transaction)
        .await
        .expect("insert text message");

    let rows = sqlx::query(
        "SELECT m.body, ch.channel_kind, cm.role
         FROM messages m
         JOIN channels ch ON m.channel_id = ch.id
         JOIN community_memberships cm
             ON cm.community_id = ch.community_id AND cm.user_id = m.author_id
         WHERE m.channel_id = $1",
    )
    .bind(channel_id)
    .fetch_all(&mut *transaction)
    .await
    .expect("read joined message");
    assert_eq!(rows.len(), 1);
    assert_eq!(
        rows[0].try_get::<String, _>("body").expect("body"),
        "Hello from migration smoke test"
    );
    assert_eq!(
        rows[0].try_get::<String, _>("channel_kind").expect("kind"),
        "text"
    );

    transaction.rollback().await.expect("leave no data behind");
}
