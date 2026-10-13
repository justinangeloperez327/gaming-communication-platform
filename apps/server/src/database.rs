//! PostgreSQL connection pooling and explicitly controlled schema migrations.
//! Never log the DATABASE_URL: it may contain credentials.

use std::{env, io, time::Duration};

use sqlx::{PgPool, migrate::Migrator, postgres::PgPoolOptions};

pub static MIGRATOR: Migrator = sqlx::migrate!("./migrations");

fn config_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

fn optional_flag(name: &str) -> Result<bool, io::Error> {
    match env::var(name) {
        Ok(raw) if raw == "1" => Ok(true),
        Ok(raw) if raw == "0" => Ok(false),
        Ok(_) => Err(config_error("Database boolean flags must be 0 or 1")),
        Err(env::VarError::NotPresent) => Ok(false),
        Err(_) => Err(config_error("Invalid database flag environment value")),
    }
}

fn pool_connections(raw: &str) -> Result<u32, io::Error> {
    match raw.parse::<u32>() {
        Ok(value @ 1..=32) => Ok(value),
        _ => Err(config_error(
            "GCP_DB_MAX_CONNECTIONS must be between 1 and 32",
        )),
    }
}

/// The API can run in health-only development mode without DATABASE_URL.
/// If GCP_DATABASE_REQUIRED=1, missing credentials are a startup error.
pub async fn connect_from_env() -> Result<Option<PgPool>, Box<dyn std::error::Error + Send + Sync>>
{
    let required = optional_flag("GCP_DATABASE_REQUIRED")?;
    let migrate = optional_flag("GCP_RUN_MIGRATIONS")?;
    let url = match env::var("DATABASE_URL") {
        Ok(value) => value,
        Err(env::VarError::NotPresent) if !required && !migrate => return Ok(None),
        Err(env::VarError::NotPresent) => {
            return Err(config_error("DATABASE_URL is required but missing").into());
        }
        Err(_) => return Err(config_error("DATABASE_URL is not valid Unicode").into()),
    };

    // Validate without reflecting any credential-bearing input in exceptions.
    if !(url.starts_with("postgres://") || url.starts_with("postgresql://")) {
        return Err(config_error("DATABASE_URL must use a PostgreSQL connection URL").into());
    }
    let max_connections = match env::var("GCP_DB_MAX_CONNECTIONS") {
        Ok(value) => pool_connections(&value)?,
        Err(env::VarError::NotPresent) => 10,
        Err(_) => return Err(config_error("Invalid DB pool size environment value").into()),
    };

    let pool = PgPoolOptions::new()
        .max_connections(max_connections)
        .min_connections(1)
        .acquire_timeout(Duration::from_secs(5))
        .connect(&url)
        .await?;

    // Schema changes are EXPLICIT opt-in. Run them in deployment/migration
    // jobs; avoid automatic concurrent schema changes on all replicas.
    if migrate {
        MIGRATOR.run(&pool).await?;
    }
    Ok(Some(pool))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pool_size_must_be_bounded() {
        assert_eq!(pool_connections("1").expect("minimum"), 1);
        assert_eq!(pool_connections("32").expect("maximum"), 32);
        for invalid in ["0", "33", "nan", "-1", ""] {
            assert!(pool_connections(invalid).is_err());
        }
    }

    #[test]
    fn migration_bundle_is_not_empty() {
        assert!(!MIGRATOR.migrations.is_empty());
    }
}
