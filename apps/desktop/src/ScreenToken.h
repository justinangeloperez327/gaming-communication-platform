#pragma once
#include "../../../native/transport/src/screen_control.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
inline bool screenTokenMatches(const QString& token, const QString& identity, const QString& room, bool publish) {
    const auto encoded = token.toLatin1();
    if (!gcp::transport::screen_jwt(encoded.toStdString())) return false;
    const auto parts = encoded.split('.');
    const auto jwt = QJsonDocument::fromJson(QByteArray::fromBase64(parts[1], QByteArray::Base64UrlEncoding)).object();
    const auto video = jwt.value("video").toObject();
    const auto sources = video.value("canPublishSources").toArray();
    const auto disabled = [&](const char* key) { const auto value = video.value(key); return value.isUndefined() || value == QJsonValue(false); };
    return video.value("canPublishSources").isArray() && jwt.value("sub").toString() == identity && video.value("room").toString() == room &&
        video.value("roomJoin") == QJsonValue(true) && video.value("canPublish") == QJsonValue(publish) &&
        video.value("canSubscribe") == QJsonValue(!publish) && video.value("canPublishData") == QJsonValue(false) &&
        (publish ? sources.size() == 1 && sources[0].toString() == QStringLiteral("screen_share") : sources.isEmpty()) &&
        disabled("roomAdmin") && disabled("roomCreate") && disabled("roomList") && disabled("roomRecord") && disabled("canUpdateOwnMetadata");
}
