#pragma once
#include <QNetworkReply>
#include <QObject>

// Enforce the limit while data arrives, before finished/readAll can allocate an
// arbitrarily large response. A one-byte sentinel detects chunked responses too.
inline void boundReply(QNetworkReply *reply, qint64 maximum) {
    reply->setReadBufferSize(maximum + 1);
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply, maximum]() {
        if (reply->bytesAvailable() > maximum) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, maximum]() {
        bool valid = false;
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
        if (valid && length > maximum) reply->abort();
    });
}
