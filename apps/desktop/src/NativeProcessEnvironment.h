#pragma once
#include <QProcessEnvironment>

// Shared voice/screen child policy. Keep OS runtime/device variables while
// excluding known server/account credentials inherited from a developer shell.
// Windows variable names are case-insensitive; remove their actual spelling.
inline QProcessEnvironment nativeProcessEnvironment() {
    auto result = QProcessEnvironment::systemEnvironment();
    for (const auto& name : result.keys()) {
        const auto upper = name.toUpper();
        if (upper == "DATABASE_URL" || upper == "PGPASSWORD" || upper == "PGPASSFILE" ||
            upper.startsWith("LIVEKIT_") || upper.startsWith("GCP_LIVEKIT_") ||
            upper.startsWith("GCP_MEDIA_") || upper.startsWith("GCP_POSTGRES_") ||
            upper == "GCP_ACCESS_TOKEN" || upper == "GCP_REFRESH_TOKEN" ||
            upper == "GCP_BOOTSTRAP_TOKEN" || upper == "GCP_OWNER_PASSWORD") result.remove(name);
    }
    return result;
}
