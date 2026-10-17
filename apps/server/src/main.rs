use std::{env, future::IntoFuture};

use gaming_comms_server::{database, livekit_tokens::LiveKitConfig, router_with_livekit};
use tokio::net::TcpListener;
use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new("gaming_comms_server=info"));
    tracing_subscriber::fmt().with_env_filter(filter).init();

    let database = database::connect_from_env().await?;
    if database.is_none() {
        tracing::warn!("PostgreSQL not configured; /health is live but /ready reports unavailable");
    }

    let bind_address =
        env::var("GCP_BIND_ADDRESS").unwrap_or_else(|_| "127.0.0.1:8080".to_string());
    let listener = TcpListener::bind(&bind_address).await?;

    tracing::info!(address = %bind_address, "HTTP server listening");

    let livekit = LiveKitConfig::from_env()?;
    let eviction = async {
        match (&database, &livekit) {
            (Some(pool), Some(config)) => config.run_eviction_loop(pool).await,
            _ => std::future::pending::<()>().await,
        }
    };
    let server = axum::serve(listener, router_with_livekit(database.clone(), livekit.clone()))
        .with_graceful_shutdown(async {
            if tokio::signal::ctrl_c().await.is_ok() {
                tracing::info!("Shutdown signal received");
            }
        })
        .into_future();
    tokio::select! {
        result = server => result?,
        () = eviction => {}
    }

    Ok(())
}
