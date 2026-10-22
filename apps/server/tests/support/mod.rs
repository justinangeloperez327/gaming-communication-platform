use sha2::{Digest, Sha256};
use sqlx::PgPool;

// Privileged SQL fixture for subsystem tests: no production registration bypass.
// Owner-only issuance/HTTP authorization is exercised in private_onboarding.rs.
pub async fn invitation(pool: &PgPool, email: &str) -> String {
    let token = format!(
        "{:032x}{:032x}",
        rand::random::<u128>(),
        rand::random::<u128>()
    );
    let digest = format!("{:x}", Sha256::digest(token.as_bytes()));
    sqlx::query("INSERT INTO account_invitations (email, token_hash) VALUES ($1, $2)")
        .bind(email.to_ascii_lowercase())
        .bind(digest)
        .execute(pool)
        .await
        .expect("privileged test invitation fixture");
    token
}
