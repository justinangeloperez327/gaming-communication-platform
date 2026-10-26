#include "ScreenSession.h"
#include "ScreenToken.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatProtocol.h"
#include "BoundedReply.h"
#include "NativeProcessEnvironment.h"
#include "../../../native/transport/src/screen_control.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <cmath>
#include <utility>

namespace {
using gcp::transport::screen_now_ms;
QString failure(int code) {
    if (code == 401) return QStringLiteral("Screen sharing stopped: sign in again, then choose a source.");
    if (code == 403 || code == 404) return QStringLiteral("Screen sharing stopped: access to this channel was removed.");
    if (code == 409) return QStringLiteral("Screen sharing stopped: the lease expired or another share is active. Wait a minute, then choose a source.");
    if (code == 503) return QStringLiteral("Screen sharing stopped: the media service is not ready. Choose a source again when it recovers.");
    return QStringLiteral("Screen sharing stopped: authorization failed. Choose a source again to retry.");
}
}
ScreenSession::ScreenSession(AccountSession* account, ChannelDirectory* directory, QObject* parent, ScreenRuntime runtime)
    : QObject(parent), account_(account), directory_(directory), runtime_(std::move(runtime)) {
    runtime_.renewMs = qBound(100, runtime_.renewMs, 20000);
    runtime_.requestMs = qBound(100, runtime_.requestMs, 10000);
    runtime_.startMs = qBound(100, runtime_.startMs, 20000);
    network_.setProxy(QNetworkProxy::NoProxy);
    for (auto* timer : {&renewal_, &deadline_, &startup_, &stopWatchdog_}) timer->setSingleShot(true);
    heartbeat_.setInterval(1000);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        if (!sameScope()) { stopWithReason(QStringLiteral("Screen sharing stopped: account or channel changed.")); return; }
        if (!control("ping")) stopWithReason(QStringLiteral("Screen sharing stopped: the capture process is unavailable."));
    });
    connect(&renewal_, &QTimer::timeout, this, [this] { requestGrant(!lease_.isEmpty()); });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        stopWithReason(QStringLiteral("Screen sharing stopped: authorization expired. Choose a source again."));
    });
    connect(&startup_, &QTimer::timeout, this, [this] {
        stopWithReason(QStringLiteral("Screen sharing stopped: capture did not start. Check Windows permissions and hardware H.264 support, then choose a source again."));
    });
    connect(&stopWatchdog_, &QTimer::timeout, this, [this] { process_.kill(); });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &ScreenSession::readOutput);
    connect(&process_, &QProcess::started, this, [this] {
        if (stopping_) { control("stop"); process_.closeWriteChannel(); return; }
        heartbeat_.start(); control("ping");
    });
    connect(&process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { processFinished(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            stopWithReason(QStringLiteral("Screen capture could not start. Check the app's Windows dependencies."));
            processFinished();
        }
    });
    connect(account_, &AccountSession::changed, this, &ScreenSession::contextChanged);
    connect(account_, &AccountSession::signedOut, this, &ScreenSession::contextChanged);
    connect(directory_, &ChannelDirectory::selectionChanged, this, &ScreenSession::contextChanged);
    connect(directory_, &ChannelDirectory::changed, this, &ScreenSession::contextChanged);
}
ScreenSession::~ScreenSession() {
    ++generation_; heartbeat_.stop(); renewal_.stop(); deadline_.stop(); startup_.stop(); stopWatchdog_.stop();
    abortPending();
    for (auto* reply : network_.findChildren<QNetworkReply*>()) { reply->disconnect(this); reply->abort(); }
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(2000); }
}
QString ScreenSession::program() const {
    return runtime_.program.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-screen.exe")) : runtime_.program;
}
bool ScreenSession::available() const { return QFileInfo(program()).isFile(); }
bool ScreenSession::busy() const { return process_.state() != QProcess::NotRunning || pending_ || cleanup_; }
QString ScreenSession::path() const { return QStringLiteral("/v1/communities/%1/channels/%2/screen-sessions").arg(community_, channel_); }
bool ScreenSession::sameScope() const {
    return account_->signedIn() && account_->apiOrigin() == origin_ && account_->userId() == user_ &&
        directory_->communityId() == community_ && directory_->channelId() == channel_;
}
void ScreenSession::contextChanged() {
    if (busy() && !stopping_ && !sameScope()) stopWithReason(QStringLiteral("Screen sharing stopped: account or channel changed."));
    if (sameScope()) access_ = account_->accessToken(); // Same login's refresh, never a new consent.
    emit changed();
}
void ScreenSession::chooseSource() {
    if (busy()) return;
    if (!available() || !account_->signedIn() || account_->busy() || directory_->busy() ||
        !ChatProtocol::originValid(account_->apiOrigin()) || !ChatProtocol::uuid(account_->userId()) ||
        !ChatProtocol::uuid(directory_->communityId()) || !ChatProtocol::uuid(directory_->channelId())) {
        status_ = QStringLiteral("Sign in and choose a voice channel. The Windows screen publisher must be installed."); emit changed(); return;
    }
    ++generation_; stopping_ = false; stopReason_.clear(); consent_ = false; sharing_ = false;
    connected_ = false; capturing_ = false; awaitingConsent_ = false; leaseDeadline_ = 0;
    origin_ = account_->apiOrigin(); user_ = account_->userId(); access_ = account_->accessToken();
    community_ = directory_->communityId(); channel_ = directory_->channelId();
    scopeLabel_ = directory_->communityName() + QStringLiteral(" / ") + directory_->channelName();
    sourceName_.clear(); width_ = height_ = 0; output_.clear();
    process_.setProcessEnvironment(nativeProcessEnvironment());
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setStandardErrorFile(QProcess::nullDevice());
    process_.setProgram(program()); process_.setArguments({QStringLiteral("--pick-source")});
    status_ = QStringLiteral("Choose a window or display in the Windows picker. Capture starts after confirmation.");
    process_.start(); emit changed();
}
void ScreenSession::confirmShare(bool consent) {
    if (!awaitingConsent_ || stopping_) return;
    if (!consent || !sameScope()) { stop(); return; }
    awaitingConsent_ = false; consent_ = true; emit consentInvalidated();
    status_ = QStringLiteral("Authorizing screen sharing…"); startup_.start(runtime_.startMs); emit changed(); requestGrant(false);
}
bool ScreenSession::control(const QByteArray& line) {
    if (process_.state() != QProcess::Running || line.size() > 12288 || process_.bytesToWrite() > 16384) return false;
    return process_.write(line + '\n') == line.size() + 1;
}
void ScreenSession::abortPending() {
    if (auto* reply = std::exchange(pending_, nullptr)) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
}
void ScreenSession::requestGrant(bool renew) {
    if (!consent_ || stopping_ || pending_ || !sameScope() || (renew && lease_.isEmpty())) return;
    // A rotation may temporarily make the old bearer invalid. Never replay a
    // create automatically: its outcome could already contain a live lease.
    if (account_->busy()) { renewal_.start(100); return; }
    QUrl url(origin_); url.setPath(path() + (renew ? QStringLiteral("/%1/renew").arg(lease_) : QString{}));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + account_->accessToken().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(runtime_.requestMs);
    const auto requestedAt = screen_now_ms();
    auto* reply = network_.post(request, renew ? QByteArrayLiteral("{\"consent\":true}") : QByteArrayLiteral("{\"purpose\":\"publish\",\"consent\":true}"));
    pending_ = reply; boundReply(reply, 16384, runtime_.requestMs);
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, renew, requestedAt, generation] {
        if (generation != generation_ || pending_ != reply) { reply->deleteLater(); return; }
        pending_ = nullptr;
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto bytes = reply->readAll(); const auto error = reply->error(); reply->deleteLater();
        // Preserve a valid returned ID solely to clean up an otherwise invalid grant.
        if (!renew && code == 201) {
            const auto id = QJsonDocument::fromJson(bytes).object().value("id").toString();
            if (ChatProtocol::uuid(id)) lease_ = id;
        }
        const bool valid = error == QNetworkReply::NoError && code == (renew ? 200 : 201) &&
            sameScope() && bytes.size() <= 16384 && applyGrant(bytes, renew, requestedAt);
        bytes.fill(0);
        if (!valid) stopWithReason(failure(code));
        emit changed();
    });
}
bool ScreenSession::applyGrant(const QByteArray& bytes, bool renew, qint64 requestedAt) {
    const auto json = QJsonDocument::fromJson(bytes).object();
    const auto id = json.value("id").toString();
    const auto identity = QStringLiteral("gcp-screen-publish-") + id;
    const auto room = QStringLiteral("gcp-screen-") + channel_;
    const auto url = json.value("server_url").toString();
    auto token = json.value("participant_token").toString();
    const auto seconds = json.value("expires_in").toDouble(-1);
    if (!ChatProtocol::uuid(id) || id != lease_ || json.value("purpose").toString() != QStringLiteral("publish") ||
        json.value("participant_identity").toString() != identity || json.value("room").toString() != room ||
        !gcp::transport::permitted_livekit_url(url.toStdString()) || !screenTokenMatches(token, identity, room, true) ||
        !std::isfinite(seconds) || seconds < 1 || seconds > 60 || std::floor(seconds) != seconds) return false;
    const auto expires = requestedAt + static_cast<qint64>(seconds * 1000);
    const auto remaining = expires - screen_now_ms();
    if (remaining < 100 || (renew && screen_now_ms() >= leaseDeadline_)) return false;
    const auto command = renew ? "renew " + QByteArray::number(expires) :
        "publish " + QByteArray::number(expires) + " " + url.toUtf8() + " " + token.toUtf8() + " " + room.toUtf8() + " " + identity.toUtf8();
    const bool sent = control(command); token.fill(QChar(0));
    if (!sent) return false;
    leaseDeadline_ = expires; deadline_.start(static_cast<int>(remaining));
    renewal_.start(qMin(runtime_.renewMs, qMax(100, static_cast<int>(remaining / 3))));
    if (!renew) { status_ = QStringLiteral("Starting screen capture…"); startup_.start(runtime_.startMs); }
    return true;
}
void ScreenSession::readOutput() {
    if (stopping_) { process_.closeReadChannel(QProcess::StandardOutput); return; }
    int drained = 0;
    while (process_.bytesAvailable() > 0) {
        const auto bytes = process_.read(qMin<qint64>(4096, process_.bytesAvailable()));
        drained += bytes.size(); output_ += bytes;
        if (drained > 65536 || output_.size() > 8192) { stopWithReason(QStringLiteral("Screen capture stopped: invalid process output.")); return; }
        qsizetype newline = -1;
        while ((newline = output_.indexOf('\n')) >= 0) {
            auto line = output_.left(newline); output_.remove(0, newline + 1);
            if (line.endsWith('\r')) line.chop(1);
            if (!line.startsWith("GCP_SCREEN ")) { stopWithReason(QStringLiteral("Screen capture stopped: invalid process response.")); return; }
            line.remove(0, 11);
            if (line.startsWith("source ") && sourceName_.isEmpty() && !consent_) {
                const auto parts = line.split(' ');
                if (parts.size() != 4 || parts[1].size() > 1368) { stop(); return; }
                const auto decoded = QByteArray::fromBase64Encoding(parts[1], QByteArray::AbortOnBase64DecodingErrors);
                const auto name = QString::fromUtf8(decoded.decoded);
                bool valid = decoded.decodingStatus == QByteArray::Base64DecodingStatus::Ok && !name.isEmpty() && name.toUtf8() == decoded.decoded;
                for (const auto c : name) if (c.category() == QChar::Other_Control) valid = false;
                bool w = false, h = false; width_ = parts[2].toInt(&w); height_ = parts[3].toInt(&h);
                if (!valid || !w || !h || width_ < 160 || width_ > 1920 || height_ < 90 || height_ > 1080 || !sameScope()) { stop(); return; }
                sourceName_ = name; awaitingConsent_ = true;
                status_ = QStringLiteral("Confirm the selected source before sharing."); emit changed(); emit consentRequested();
            } else if (line == "connected" && consent_ && !lease_.isEmpty() && !connected_) {
                connected_ = true;
            } else if (line == "capturing" && connected_ && !capturing_) {
                capturing_ = true; status_ = QStringLiteral("Capturing %1; waiting for the first encoded frame…").arg(sourceName_);
            } else if (line == "sharing" && capturing_ && !sharing_) {
                sharing_ = true; startup_.stop(); status_ = QStringLiteral("Sharing %1").arg(sourceName_);
            } else if (line == "cancelled") { stopWithReason(QStringLiteral("Screen selection cancelled")); return;
            } else if (line == "unsupported_size") { stopWithReason(QStringLiteral("Choose an SDR window or display between 160×90 and 1920×1080.")); return;
            } else if (line == "failed" || line == "stopped") {
                stopWithReason(QStringLiteral("Screen sharing stopped. Check the selected source, Windows permissions and hardware H.264 support; choose a source again.")); return;
            } else { stopWithReason(QStringLiteral("Screen capture stopped: unexpected process state.")); return; }
            emit changed();
        }
    }
}
void ScreenSession::revoke() {
    if (lease_.isEmpty()) return;
    auto id = std::exchange(lease_, {});
    QUrl url(origin_); url.setPath(path() + QStringLiteral("/") + id);
    QNetworkRequest request(url); request.setRawHeader("Authorization", "Bearer " + access_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(3000);
    auto* reply = network_.deleteResource(request); cleanup_ = reply; boundReply(reply, 1024, 3000);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (cleanup_ == reply) cleanup_ = nullptr;
        const bool removed = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 204;
        reply->deleteLater();
        if (!removed) { stopReason_ += QStringLiteral(" Server cleanup could not be confirmed; its lease expires within a minute."); if (process_.state() == QProcess::NotRunning) status_ = stopReason_; }
        emit changed();
    });
}
void ScreenSession::stop() { stopWithReason(QStringLiteral("Screen sharing stopped")); }
void ScreenSession::stopWithReason(const QString& reason) {
    if (stopping_) return;
    ++generation_; stopping_ = true; stopReason_ = reason;
    awaitingConsent_ = consent_ = sharing_ = connected_ = capturing_ = false;
    heartbeat_.stop(); renewal_.stop(); deadline_.stop(); startup_.stop(); abortPending();
    emit consentInvalidated(); revoke();
    sourceName_.clear(); scopeLabel_.clear(); width_ = height_ = 0;
    access_.fill(QChar(0)); access_.clear(); user_.clear();
    community_.clear(); channel_.clear(); origin_.clear();
    if (process_.state() != QProcess::NotRunning) {
        status_ = QStringLiteral("Stopping screen capture…");
        control("stop"); process_.closeWriteChannel(); process_.closeReadChannel(QProcess::StandardOutput);
        stopWatchdog_.start(1500);
    } else status_ = stopReason_;
    output_.clear(); emit changed();
}
void ScreenSession::processFinished() {
    if (!stopping_) stopWithReason(QStringLiteral("Screen sharing stopped: the capture process closed. Choose a source again."));
    stopWatchdog_.stop(); heartbeat_.stop(); status_ = stopReason_; output_.clear(); emit changed();
}
