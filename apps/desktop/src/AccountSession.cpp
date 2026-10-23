#include "BoundedReply.h"
#include "AccountSession.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QUuid>
#include <QVariantMap>

namespace {
bool approved(const QUrl &url) {
    if (!url.isValid() || url.host().isEmpty() || url.toString().size() > 2048 ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    if (!url.path().isEmpty() && url.path() != QStringLiteral("/")) return false;
    const auto host = url.host().toLower();
    return url.scheme() == QStringLiteral("https") ||
           (url.scheme() == QStringLiteral("http") &&
            (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")));
}
bool hexToken(const QString &s) {
    if (s.size() != 64) return false;
    for (const auto c : s) {
        if ((c < QChar('0') || c > QChar('9')) && (c < QChar('a') || c > QChar('f'))) return false;
    }
    return true;
}
QString requestError(int http, QNetworkReply::NetworkError error, bool registration) {
    if (http == 401) return QStringLiteral("Email or password is incorrect, or your session expired.");
    if (http == 403 && registration) return QStringLiteral("Invitation is invalid, expired or used. Ask the owner for a new code.");
    if (http == 403) return QStringLiteral("Only the server owner can invite accounts.");
    if (http == 409) return QStringLiteral("Email or handle is already in use. Sign in or choose another handle.");
    if (http == 400 || http == 422) return QStringLiteral("Check the email, handle and password requirements.");
    if (http == 429) return QStringLiteral("Server is busy. Try again shortly; the owner may need to revoke unused invitations.");
    if (http == 503) return QStringLiteral("Server is not ready. Ask the owner to check the services.");
    if (http >= 300 && http < 400) return QStringLiteral("Server redirect blocked. Enter the final HTTPS server address.");
    if (error == QNetworkReply::SslHandshakeFailedError) return QStringLiteral("Server certificate could not be verified. Check the server address.");
    if (error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError)
        return QStringLiteral("Request timed out or exceeded its response limit. Try again.");
    if (error != QNetworkReply::NoError && http == 0)
        return QStringLiteral("Could not reach the server. Check the address and network connection.");
    return QStringLiteral("Server returned an unexpected response. Try again or contact the owner.");
}
}
AccountSession::AccountSession(QObject *parent) : QObject(parent) {
    refreshTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, &AccountSession::refresh);
}
AccountSession::~AccountSession() {
    refreshTimer_.stop();
    // Abort while our state still exists; the network member is destroyed last.
    for (auto *reply : network_.findChildren<QNetworkReply *>()) {
        reply->disconnect(this);
        reply->abort();
    }
}
void AccountSession::clearSession() {
    refreshTimer_.stop();
    access_.fill(QChar(0)); access_.clear();
    refresh_.fill(QChar(0)); refresh_.clear();
    displayName_.clear();
    userId_.clear();
    serverOwner_ = false;
    invitationCode_.fill(QChar(0)); invitationCode_.clear();
    invitationId_.clear();
    invitationEmail_.clear();
    invitations_.clear();
    invitationStatus_.clear();
    if (invitationPending_) {
        auto *reply = invitationPending_; invitationPending_ = nullptr;
        reply->abort(); reply->deleteLater();
    }
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
    sendAuth(QStringLiteral("/v1/auth/login"), payload, AuthOperation::SignIn);
    payload.fill(0);
}
void AccountSession::createAccount(const QString &origin, const QString &handle,
    const QString &displayName, const QString &email, const QString &password, const QString &invitation) {
    if (busy() || signedIn()) return;
    const QUrl url(origin);
    bool validHandle = handle.size() >= 3 && handle.size() <= 32;
    for (const auto c : handle) {
        if (!((c >= QChar('a') && c <= QChar('z')) ||
              (c >= QChar('0') && c <= QChar('9')) || c == QChar('_'))) validHandle = false;
    }
    const auto passwordBytes = password.toUtf8();
    if (!approved(url) || url.host().isEmpty()) {
        status_ = QStringLiteral("Enter an HTTPS server address, or HTTP on localhost for testing.");
        emit changed(); return;
    }
    if (!validHandle || displayName.trimmed() != displayName || displayName.isEmpty() || displayName.size() > 80 ||
        email.isEmpty() || email.size() > 320 || passwordBytes.size() < 12 || passwordBytes.size() > 128) {
        status_ = QStringLiteral("Use a 3-32 character lowercase handle, a display name, email, and a 12-128 byte password.");
        emit changed(); return;
    }
    if (!hexToken(invitation)) {
        status_ = QStringLiteral("Enter the 64-character invitation code from the server owner.");
        emit changed(); return;
    }
    origin_ = url;
    auto payload = QJsonDocument(QJsonObject{{"handle", handle}, {"display_name", displayName},
        {"email", email}, {"password", password}, {"invitation_token", invitation}}).toJson(QJsonDocument::Compact);
    sendAuth(QStringLiteral("/v1/auth/register"), payload, AuthOperation::Register);
    payload.fill(0);
}
void AccountSession::sendAuth(const QString &path, const QByteArray &payload, AuthOperation operation) {
    QUrl url = origin_;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    status_ = operation == AuthOperation::Refresh ? QStringLiteral("Refreshing session") :
        operation == AuthOperation::Register ? QStringLiteral("Creating account") : QStringLiteral("Signing in");
    pending_ = network_.post(request, payload);
    boundReply(pending_, 16384, 10000);
    emit changed();
    auto *reply = pending_;
    const quint64 id = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, operation]() {
        if (id != generation_ || pending_ != reply) { reply->deleteLater(); return; }
        pending_ = nullptr;
        const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto err = reply->error();
        auto bytes = reply->readAll();
        reply->deleteLater();
        const int expected = operation == AuthOperation::Register ? 201 : 200;
        if (err != QNetworkReply::NoError || http != expected ||
            bytes.size() > 16384 || !applyResponse(bytes, operation == AuthOperation::Refresh)) {
            clearSession();
            status_ = requestError(http, err, operation == AuthOperation::Register);
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
    if (!isRefresh) {
        displayName_ = name.left(80);
        userId_ = obj.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
    }
    serverOwner_ = obj.value(QStringLiteral("server_owner")).toBool(false);
    access_ = access;
    refresh_ = nextRefresh;
    status_ = QStringLiteral("Signed in");
    refreshTimer_.start(qMax(1000, (seconds - 60) * 1000));
    if (!isRefresh && serverOwner_) QTimer::singleShot(0, this, &AccountSession::loadInvitations);
    return true;
}
void AccountSession::refresh() {
    if (!signedIn() || busy()) return;
    QJsonObject obj{{QStringLiteral("refresh_token"), refresh_}};
    auto payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sendAuth(QStringLiteral("/v1/auth/refresh"), payload, AuthOperation::Refresh);
    payload.fill(0);
}
void AccountSession::inviteFriend(const QString &email) {
    if (!signedIn() || !serverOwner_ || invitationBusy() || busy()) return;
    if (email.isEmpty() || email.size() > 320) {
        invitationStatus_ = QStringLiteral("Enter your friend's email."); emit changed(); return;
    }
    auto payload = QJsonDocument(QJsonObject{{"email", email}}).toJson(QJsonDocument::Compact);
    sendInvitation(InvitationOperation::Create, payload);
}
void AccountSession::revokeInvitation() {
    if (!signedIn() || !serverOwner_ || invitationBusy() || invitationId_.isEmpty()) return;
    sendInvitation(InvitationOperation::Revoke, {}, invitationId_);
}
void AccountSession::revokePendingInvitation(const QString &id) {
    if (!signedIn() || !serverOwner_ || invitationBusy() || QUuid(id).isNull()) return;
    sendInvitation(InvitationOperation::Revoke, {}, QUuid(id).toString(QUuid::WithoutBraces));
}
void AccountSession::loadInvitations() {
    if (!signedIn() || !serverOwner_ || invitationBusy()) return;
    sendInvitation(InvitationOperation::List);
}
void AccountSession::sendInvitation(InvitationOperation operation, const QByteArray &payload, const QString &targetId) {
    const bool revoke = operation == InvitationOperation::Revoke;
    QUrl url = origin_;
    url.setPath(revoke ? QStringLiteral("/v1/account-invitations/") + targetId : QStringLiteral("/v1/account-invitations"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + access_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    auto *reply = revoke ? network_.deleteResource(request) :
        operation == InvitationOperation::List ? network_.get(request) : network_.post(request, payload);
    invitationPending_ = reply;
    boundReply(reply, 65536, 10000);
    invitationStatus_ = revoke ? QStringLiteral("Revoking invitation") : operation == InvitationOperation::List ?
        QStringLiteral("Loading invitations") : QStringLiteral("Creating invitation");
    const auto id = generation_;
    emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, operation, targetId]() {
        if (id != generation_ || invitationPending_ != reply || !signedIn()) { reply->deleteLater(); return; }
        invitationPending_ = nullptr;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        auto bytes = reply->readAll();
        reply->deleteLater();
        const int expected = operation == InvitationOperation::Revoke ? 204 : operation == InvitationOperation::List ? 200 : 201;
        bool valid = error == QNetworkReply::NoError && bytes.size() <= 65536 && http == expected;
        if (valid && operation == InvitationOperation::Create) {
            const auto object = QJsonDocument::fromJson(bytes).object();
            const auto code = object.value(QStringLiteral("token")).toString();
            const auto invitationId = object.value(QStringLiteral("id")).toString();
            const auto email = object.value(QStringLiteral("email")).toString();
            const int ttl = object.value(QStringLiteral("expires_in")).toInt();
            valid = hexToken(code) && !QUuid(invitationId).isNull() && !email.isEmpty() && email.size() <= 320 && ttl > 0 && ttl <= 86400;
            if (valid) {
                invitationCode_ = code;
                invitationId_ = invitationId;
                invitationEmail_ = email;
                invitationStatus_ = QStringLiteral("Share this one-use code and server address privately. It expires in 24 hours.");
            }
        } else if (valid && operation == InvitationOperation::Revoke) {
            if (targetId == invitationId_) {
                invitationCode_.fill(QChar(0)); invitationCode_.clear(); invitationId_.clear(); invitationEmail_.clear();
            }
            invitationStatus_ = QStringLiteral("Invitation revoked. Existing accounts are unaffected.");
        } else if (valid) {
            const auto document = QJsonDocument::fromJson(bytes);
            valid = document.isArray() && document.array().size() <= 128;
            QVariantList pending;
            if (valid) {
                for (const auto &value : document.array()) {
                    const auto item = value.toObject();
                    const auto invitationId = item.value(QStringLiteral("id")).toString();
                    const auto email = item.value(QStringLiteral("email")).toString();
                    if (QUuid(invitationId).isNull() || email.isEmpty() || email.size() > 320) { valid = false; break; }
                    pending.append(QVariantMap{{"id", invitationId}, {"email", email}});
                }
            }
            if (valid) { invitations_ = pending; invitationStatus_ = QStringLiteral("Pending invitations are up to date."); }
        }
        if (!valid) invitationStatus_ = requestError(http, error, false);
        bytes.fill(0);
        emit changed();
        if (valid && operation != InvitationOperation::List) QTimer::singleShot(0, this, &AccountSession::loadInvitations);
    });
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
    boundReply(reply, 16384, 5000);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}
