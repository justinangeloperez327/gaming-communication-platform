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
    return jwt.value("sub").toString() == identity && video.value("room").toString() == room &&
        video.value("roomJoin") == QJsonValue(true) && video.value("canPublish") == QJsonValue(publish) &&
        video.value("canSubscribe") == QJsonValue(!publish) && video.value("canPublishData") == QJsonValue(false) &&
        (publish ? sources.size() == 1 && sources[0].toString() == QStringLiteral("screen_share") : sources.isEmpty()) &&
        !video.value("roomAdmin").toBool() && !video.value("roomCreate").toBool() &&
        !video.value("roomList").toBool() && !video.value("roomRecord").toBool() && !video.value("canUpdateOwnMetadata").toBool();
}
