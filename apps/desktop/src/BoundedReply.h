#pragma once
#include <QNetworkReply>
#include <QObject>
#include <QTimer>

// Enforce the limit while data arrives, before finished/readAll can allocate an
// arbitrarily large response. A one-byte sentinel detects chunked responses too.
// The absolute deadline also bounds peers that keep an inactivity timeout alive
// by trickling bytes. The reply owns the timer, including cancellation cleanup.
inline void boundReply(QNetworkReply *reply, qint64 maximum, int deadlineMs) {
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, reply, [reply]() {
        if (!reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, deadline, &QTimer::stop);
    deadline->start(deadlineMs);
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
