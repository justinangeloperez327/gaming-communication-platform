#include "ServerProbe.h"
#include "BoundedReply.h"
#include "ChatProtocol.h"
#include <QNetworkRequest>
#include <utility>

ServerProbe::ServerProbe(QObject* parent, int timeoutMs)
    : QObject(parent), timeoutMs_(qBound(100, timeoutMs, 5000)) {}
ServerProbe::~ServerProbe() {
    if (pending_) { pending_->disconnect(this); pending_->abort(); }
}
void ServerProbe::clear() {
    if (auto* reply = std::exchange(pending_, nullptr)) {
        reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    status_.clear(); result_ = QStringLiteral("idle"); emit changed();
}
void ServerProbe::check(const QString& origin) {
    clear(); QUrl url(origin);
    if (origin.size() > 2048 || !ChatProtocol::originValid(url)) {
        result_ = QStringLiteral("invalid");
        status_ = QStringLiteral("Enter the final HTTPS server address with no path, credentials or query. HTTP is allowed only on localhost.");
        emit changed(); return;
    }
    url.setPath(QStringLiteral("/ready"));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(timeoutMs_);
    auto* reply = network_.get(request); pending_ = reply; boundReply(reply, 1024, timeoutMs_);
    status_ = QStringLiteral("Checking server readiness…"); result_ = QStringLiteral("checking"); emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (pending_ != reply) { reply->deleteLater(); return; }
        pending_ = nullptr;
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        if (code == 204 && error == QNetworkReply::NoError) {
            result_ = QStringLiteral("ready");
            status_ = QStringLiteral("Server is ready for sign-in. Voice and screen connectivity still need a call.");
        } else if (code == 503) {
            result_ = QStringLiteral("not_ready");
            status_ = QStringLiteral("Server is reachable but not ready. Ask the host to check the database and media service.");
        } else if (error == QNetworkReply::SslHandshakeFailedError) {
            result_ = QStringLiteral("certificate");
            status_ = QStringLiteral("Server certificate could not be verified. Check the address, Windows clock and host certificate; verification remains enabled.");
        } else if (code >= 300 && code < 400) {
            result_ = QStringLiteral("redirect");
            status_ = QStringLiteral("Redirect blocked. Ask the host for the final HTTPS server address.");
        } else if (code == 0 || error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError) {
            result_ = QStringLiteral("unreachable");
            status_ = QStringLiteral("Could not verify the server. Check the address/network and try Check connection again.");
        } else {
            result_ = QStringLiteral("unexpected");
            status_ = QStringLiteral("This address did not return GCP readiness. Check the server address with the host.");
        }
        reply->deleteLater(); emit changed();
    });
}
