use std::{
    env,
    future::IntoFuture,
    io::{self, Read},
    time::Duration,
};

use gaming_comms_server::{database, livekit_tokens::LiveKitConfig, router_with_livekit};
use tokio::net::TcpListener;
use tracing_subscriber::EnvFilter;

fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    // Health probes and migrations do not need a worker thread per host CPU.
    let maintenance = matches!(
        env::args().nth(1).as_deref(),
        Some("--check-ready" | "--migrate-only" | "--bootstrap-owner")
    );
    let mut runtime = if maintenance {
        tokio::runtime::Builder::new_current_thread()
    } else {
        tokio::runtime::Builder::new_multi_thread()
    };
    runtime.enable_all().build()?.block_on(run())
}

async fn run() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new("gaming_comms_server=info"));
    tracing_subscriber::fmt().with_env_filter(filter).init();

    let arguments: Vec<_> = env::args().skip(1).collect();
    match arguments.as_slice() {
        [argument] if argument == "--check-ready" => {
            let address =
                env::var("GCP_BIND_ADDRESS").unwrap_or_else(|_| "127.0.0.1:8080".to_string());
            let response = reqwest::Client::builder()
                .no_proxy()
                .redirect(reqwest::redirect::Policy::none())
                .timeout(Duration::from_secs(8))
                .build()?
                .get(format!("http://{address}/ready"))
                .send()
                .await?;
            if response.status() != reqwest::StatusCode::NO_CONTENT {
                return Err(io::Error::other("Server is not ready").into());
            }
            return Ok(());
        }
        [argument] if argument == "--migrate-only" => {
            let pool = database::connect_from_env()
                .await?
                .ok_or_else(|| io::Error::other("Migration requires DATABASE_URL"))?;
            database::MIGRATOR.run(&pool).await?;
            pool.close().await;
            tracing::info!("Database migrations completed");
            return Ok(());
        }
        [argument] if argument == "--bootstrap-owner" => {
            // Read bounded JSON from stdin so passwords never enter argv or logs.
            let mut bytes = tokio::task::spawn_blocking(|| {
                let mut bytes = Vec::new();
                io::stdin().take(8193).read_to_end(&mut bytes)?;
                Ok::<_, io::Error>(bytes)
            })
            .await??;
            if bytes.len() > 8192 {
                return Err(io::Error::other("Owner input exceeds size limit").into());
            }
            let input =
                serde_json::from_slice::<gaming_comms_server::onboarding::OwnerInput>(&bytes)
                    .map_err(|_| {
                        io::Error::other("Owner input must be a valid JSON account object")
                    });
            bytes.fill(0);
            let input = input?;
            let pool = database::connect_from_env()
                .await?
                .ok_or_else(|| io::Error::other("Owner bootstrap requires DATABASE_URL"))?;
            gaming_comms_server::onboarding::bootstrap_owner(&pool, input).await
                .map_err(|error| io::Error::other(format!("Owner bootstrap refused: {error:?}; check account details and existing ownership")))?;
            pool.close().await;
            tracing::info!("Server owner configured; sign in using the desktop");
            return Ok(());
        }
        [] => {}
        _ => {
            return Err(io::Error::other(
                "Expected --migrate-only, --check-ready, --bootstrap-owner, or no arguments",
            )
            .into());
        }
    }

    #[cfg(unix)]
    let mut terminate = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;

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
    let server = axum::serve(
        listener,
        router_with_livekit(database.clone(), livekit.clone()),
    )
    .with_graceful_shutdown(async move {
        #[cfg(unix)]
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {},
            _ = terminate.recv() => {},
        }
        #[cfg(not(unix))]
        let _ = tokio::signal::ctrl_c().await;
        tracing::info!("Shutdown signal received; draining HTTP connections");
    })
    .into_future();
    tokio::select! {
        result = server => result?,
        () = eviction => {}
    }

    Ok(())
}
