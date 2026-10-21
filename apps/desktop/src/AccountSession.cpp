#include "BoundedReply.h"
#include "AccountSession.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QProcessEnvironment>

namespace {
bool approved(const QUrl &url) {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    if (!url.path().isEmpty() && url.path() != QStringLiteral("/")) return false;
    const auto host = url.host().toLower();
    return url.scheme() == QStringLiteral("https") ||
           (url.scheme() == QStringLiteral("http") &&
            (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")));
}
bool hexToken(const QString &s) {
    if (s.size() != 64) return false;
    for (const auto c : s) {
        if (!c.isDigit() && (c < QChar('a') || c > QChar('f'))) return false;
    }
    return true;
}
}
AccountSession::AccountSession(QObject *parent) : QObject(parent) {
    refreshTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, &AccountSession::refresh);
}
void AccountSession::clearSession() {
    refreshTimer_.stop();
    access_.fill(QChar(0)); access_.clear();
    refresh_.fill(QChar(0)); refresh_.clear();
    displayName_.clear();
}
void AccountSession::signIn(const QString &origin, const QString &email, const QString &password) {
    if (busy() || signedIn()) return;
    const QUrl url(origin);
    if (!approved(url) || email.size() > 320 || email.isEmpty() ||
        password.isEmpty() || password.toUtf8().size() > 128) {
        status_ = QStringLiteral("Invalid server or login details"); emit changed(); return;
    }
    origin_ = url;
    QJsonObject json{{QStringLiteral("email"), email}, {QStringLiteral("password"), password}};
    auto payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    sendAuth(QStringLiteral("/v1/auth/login"), payload, false);
    payload.fill(0);
}
void AccountSession::sendAuth(const QString &path, const QByteArray &payload, bool isRefresh) {
    QUrl url = origin_;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    status_ = isRefresh ? QStringLiteral("Refreshing session") : QStringLiteral("Signing in");
    pending_ = network_.post(request, payload);
    boundReply(pending_, 16384);
    emit changed();
    auto *reply = pending_;
    const quint64 id = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, isRefresh]() {
        if (id != generation_ || pending_ != reply) { reply->deleteLater(); return; }
        pending_ = nullptr;
        const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto err = reply->error();
        auto bytes = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || http != 200 ||
            bytes.size() > 16384 || !applyResponse(bytes, isRefresh)) {
            clearSession();
            status_ = QStringLiteral("Authentication failed or session expired");
            emit signedOut();
        }
        bytes.fill(0);
        emit changed();
    });
}
bool AccountSession::applyResponse(const QByteArray &bytes, bool isRefresh) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    auto obj = doc.object();
    QString access = obj.value(QStringLiteral("access_token")).toString();
    QString nextRefresh = obj.value(QStringLiteral("refresh_token")).toString();
    const int seconds = obj.value(QStringLiteral("expires_in")).toInt();
    if (!hexToken(access) || !hexToken(nextRefresh) || seconds < 60 || seconds > 86400) return false;
    QString name = obj.value(QStringLiteral("user")).toObject().value(QStringLiteral("display_name")).toString();
    if (!isRefresh) displayName_ = name.left(80);
    access_ = access;
    refresh_ = nextRefresh;
    status_ = QStringLiteral("Signed in");
    refreshTimer_.start(qMax(1000, (seconds - 60) * 1000));
    return true;
}
void AccountSession::refresh() {
    if (!signedIn() || busy()) return;
    QJsonObject obj{{QStringLiteral("refresh_token"), refresh_}};
    auto payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendAuth(QStringLiteral("/v1/auth/refresh"), payload, true);
    payload.fill(0);
}
void AccountSession::signOut() {
    // Clear local access immediately; revoke refresh token remotely when possible.
    const QString bearer = access_;
    const QUrl origin = origin_;
    ++generation_;
    if (pending_) { auto *reply = pending_; pending_ = nullptr; reply->abort(); reply->deleteLater(); }
    clearSession();
    status_ = QStringLiteral("Signed out");
    emit signedOut();
    emit changed();
    if (bearer.isEmpty() || !approved(origin)) return;
    QUrl url = origin;
    url.setPath(QStringLiteral("/v1/auth/logout"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + bearer.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(5000);
    auto *reply = network_.post(request, QByteArray());
    // Logout has no useful response body; still bound error/proxy responses.
    boundReply(reply, 16384);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}
