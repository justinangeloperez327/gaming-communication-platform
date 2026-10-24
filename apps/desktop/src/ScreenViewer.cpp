#include "ScreenViewer.h"
#include "ScreenToken.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatProtocol.h"
#include "NativeProcessEnvironment.h"
#include "BoundedReply.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QSet>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
QString denied(int status) {
    if (status == 401) return QStringLiteral("Viewing stopped: sign in again.");
    if (status == 403 || status == 404) return QStringLiteral("Viewing stopped: this share or your channel access ended. Refresh shares.");
    if (status == 409) return QStringLiteral("Viewing stopped: a viewer lease is already active. Wait a minute, then retry.");
    return QStringLiteral("Viewing stopped: authorization or media service unavailable. Refresh shares to retry.");
}
using gcp::transport::screen_now_ms;
}
ScreenViewer::ScreenViewer(AccountSession* account, ChannelDirectory* directory, QObject* parent, ViewerRuntime runtime)
    : QObject(parent), account_(account), directory_(directory), runtime_(std::move(runtime)) {
    runtime_.renewMs = qBound(100, runtime_.renewMs, 20000);
    runtime_.requestMs = qBound(100, runtime_.requestMs, 10000);
    runtime_.startMs = qBound(100, runtime_.startMs, 20000);
    runtime_.frameMs = qBound(33, runtime_.frameMs, 1000);
    network_.setProxy(QNetworkProxy::NoProxy);
    for (auto* timer : {&renewal_, &deadline_, &startup_, &kill_, &frameTimer_, &payloadDeadline_}) timer->setSingleShot(true);
    heartbeat_.setInterval(1000);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        if (!sameScope() || !control("ping")) stopWithReason(QStringLiteral("Viewing stopped: account/channel or media process changed."));
    });
    connect(&renewal_, &QTimer::timeout, this, [this] { requestGrant(!lease_.isEmpty()); });
    connect(&deadline_, &QTimer::timeout, this, [this] { stopWithReason(QStringLiteral("Viewing stopped: authorization expired.")); });
    connect(&startup_, &QTimer::timeout, this, [this] { stopWithReason(QStringLiteral("Viewing stopped: no screen frame arrived. Refresh shares and try again.")); });
    connect(&payloadDeadline_, &QTimer::timeout, this, [this] { stopWithReason(QStringLiteral("Viewing stopped: incomplete video frame.")); });
    connect(&kill_, &QTimer::timeout, this, [this] { process_.kill(); });
    connect(&frameTimer_, &QTimer::timeout, this, &ScreenViewer::requestFrame);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &ScreenViewer::readOutput);
    connect(&process_, &QProcess::started, this, [this] {
        if (stopping_) { control("stop"); process_.closeWriteChannel(); return; }
        heartbeat_.start(); control("ping"); requestGrant(false);
    });
    connect(&process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { processFinished(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { stopWithReason(QStringLiteral("Screen viewer could not start. Check Windows dependencies.")); processFinished(); }
    });
    connect(account_, &AccountSession::changed, this, &ScreenViewer::contextChanged);
    connect(account_, &AccountSession::signedOut, this, &ScreenViewer::contextChanged);
    connect(directory_, &ChannelDirectory::changed, this, &ScreenViewer::contextChanged);
    connect(directory_, &ChannelDirectory::selectionChanged, this, &ScreenViewer::contextChanged);
    contextChanged();
}
ScreenViewer::~ScreenViewer() {
    ++generation_;
    for (auto* timer : {&heartbeat_, &renewal_, &deadline_, &startup_, &kill_, &frameTimer_, &payloadDeadline_}) timer->stop();
    for (auto* reply : network_.findChildren<QNetworkReply*>()) { reply->disconnect(this); reply->abort(); }
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(2000); }
}
QString ScreenViewer::program() const { return runtime_.program.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-screen.exe")) : runtime_.program; }
bool ScreenViewer::available() const { return QFileInfo(program()).isFile(); }
bool ScreenViewer::busy() const { return consent_ || process_.state() != QProcess::NotRunning || pending_ || listing_ || cleanup_; }
QString ScreenViewer::path() const { return QStringLiteral("/v1/communities/%1/channels/%2/screen-sessions").arg(community_, channel_); }
bool ScreenViewer::sameScope() const {
    return account_->signedIn() && account_->apiOrigin() == origin_ && account_->userId() == user_ && directory_->communityId() == community_ && directory_->channelId() == channel_;
}
void ScreenViewer::abort(QNetworkReply*& pending) { if (auto* reply = std::exchange(pending, nullptr)) { reply->disconnect(this); reply->abort(); reply->deleteLater(); } }
void ScreenViewer::contextChanged() {
    if (!sameScope()) {
        stopWithReason(QStringLiteral("Viewing stopped: account or channel changed."));
        ++generation_; abort(listing_); publishers_.clear(); selected_.clear(); publisherName_.clear();
        origin_ = account_->apiOrigin(); user_ = account_->userId(); community_ = directory_->communityId(); channel_ = directory_->channelId();
        scopeLabel_ = directory_->communityName() + QStringLiteral(" / ") + directory_->channelName();
    }
    if (account_->signedIn() && sameScope()) access_ = account_->accessToken(); else { access_.fill(QChar(0)); access_.clear(); }
    emit changed();
}
void ScreenViewer::refreshPublishers() {
    if (busy() || !sameScope() || account_->busy() || directory_->busy() || !ChatProtocol::originValid(origin_) || !ChatProtocol::uuid(community_) || !ChatProtocol::uuid(channel_)) return;
    QUrl url(origin_); url.setPath(path()); QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + account_->accessToken().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(runtime_.requestMs);
    auto* reply = network_.get(request); listing_ = reply; boundReply(reply, 262144, runtime_.requestMs);
    const auto generation = generation_; status_ = QStringLiteral("Loading channel shares…"); emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        if (listing_ != reply || generation != generation_) { reply->deleteLater(); return; }
        listing_ = nullptr;
        const auto bytes = reply->readAll(); const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto doc = QJsonDocument::fromJson(bytes);
        bool valid = reply->error() == QNetworkReply::NoError && code == 200 && sameScope() && bytes.size() <= 262144 && doc.isArray() && doc.array().size() <= 200;
        reply->deleteLater(); QVariantList entries; QSet<QString> ids;
        for (const auto& value : doc.array()) {
            const auto row = value.toObject(); const auto id = row.value("id").toString(), user = row.value("user_id").toString(), name = row.value("display_name").toString();
            valid = valid && ChatProtocol::uuid(id) && ChatProtocol::uuid(user) && !ids.contains(id) && !name.isEmpty() && name.size() <= 80 && row.value("participant_identity").toString() == QStringLiteral("gcp-screen-publish-") + id;
            for (auto c : name) if (c.category() == QChar::Other_Control) valid = false;
            ids.insert(id); entries.append(QVariantMap{{"id", id}, {"name", name}});
        }
        publishers_ = valid ? entries : QVariantList{};
        if (!ids.contains(selected_) || !valid) { selected_.clear(); publisherName_.clear(); }
        status_ = valid ? entries.isEmpty() ? QStringLiteral("No active shares in this voice channel") : QStringLiteral("Choose a publisher and select Watch") : denied(code);
        emit changed();
    });
}
void ScreenViewer::choosePublisher(const QString& id) {
    if (busy()) return;
    for (const auto& item : publishers_) if (item.toMap().value("id").toString() == id) {
        selected_ = id; publisherName_ = item.toMap().value("name").toString(); emit changed(); return;
    }
}
void ScreenViewer::watch(bool consent) {
    if (busy() || !consent || !available() || !sameScope() || account_->busy() || directory_->busy() || !ChatProtocol::uuid(selected_) || !ChatProtocol::originValid(origin_) || !ChatProtocol::uuid(user_)) return;
    bool found = false; for (const auto& item : publishers_) found = found || item.toMap().value("id").toString() == selected_;
    if (!found) return;
    ++generation_; stopping_ = false; consent_ = true; watching_ = connected_ = subscribed_ = credit_ = false;
    frameNumber_ = 0; rotation_ = 0; image_ = {}; header_.reset(); pixels_.clear(); line_.clear(); trackSid_.clear();
    access_ = account_->accessToken();
    process_.setProcessEnvironment(nativeProcessEnvironment()); process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setStandardErrorFile(QProcess::nullDevice()); process_.setProgram(program()); process_.setArguments({QStringLiteral("--view")});
    status_ = QStringLiteral("Authorizing viewer…"); startup_.start(runtime_.startMs);
    emit started(); process_.start(); emit changed();
}
bool ScreenViewer::control(const QByteArray& line) {
    return process_.state() == QProcess::Running && line.size() <= 12288 && process_.bytesToWrite() <= 16384 && process_.write(line + '\n') == line.size() + 1;
}
void ScreenViewer::requestGrant(bool renew) {
    if (!consent_ || stopping_ || pending_ || !sameScope()) return;
    if (account_->busy()) { renewal_.start(100); return; }
    QUrl url(origin_); url.setPath(path() + (renew ? QStringLiteral("/%1/renew").arg(lease_) : QString{}));
    QNetworkRequest request(url); request.setRawHeader("Authorization", "Bearer " + account_->accessToken().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(runtime_.requestMs);
    const auto requestedAt = screen_now_ms(); const auto generation = generation_;
    auto* reply = network_.post(request, renew ? QByteArrayLiteral("{\"consent\":true}") : QByteArrayLiteral("{\"purpose\":\"view\",\"consent\":true}"));
    pending_ = reply; boundReply(reply, 16384, runtime_.requestMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, renew, requestedAt] {
        if (pending_ != reply || generation != generation_) { reply->deleteLater(); return; }
        pending_ = nullptr; const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); auto bytes = reply->readAll();
        if (!renew && code == 201) { const auto id = QJsonDocument::fromJson(bytes).object().value("id").toString(); if (ChatProtocol::uuid(id)) lease_ = id; }
        const bool valid = reply->error() == QNetworkReply::NoError && code == (renew ? 200 : 201) && sameScope() && bytes.size() <= 16384 && applyGrant(bytes, renew, requestedAt);
        bytes.fill(0); reply->deleteLater();
        if (!valid) stopWithReason(denied(code)); else if (renew && connected_) requestTrack();
        emit changed();
    });
}
bool ScreenViewer::applyGrant(const QByteArray& bytes, bool renew, qint64 requestedAt) {
    const auto json = QJsonDocument::fromJson(bytes).object(); const auto id = json.value("id").toString();
    const auto identity = QStringLiteral("gcp-screen-view-") + id, room = QStringLiteral("gcp-screen-") + channel_;
    const auto url = json.value("server_url").toString(), token = json.value("participant_token").toString(); const auto seconds = json.value("expires_in").toDouble(-1);
    if (!ChatProtocol::uuid(id) || id != lease_ || json.value("purpose").toString() != QStringLiteral("view") || json.value("participant_identity").toString() != identity || json.value("room").toString() != room ||
        !gcp::transport::permitted_livekit_url(url.toStdString()) || !screenTokenMatches(token, identity, room, false) || !std::isfinite(seconds) || std::floor(seconds) != seconds || seconds < 1 || seconds > 60) return false;
    const auto expires = requestedAt + static_cast<qint64>(seconds * 1000); const auto remaining = expires - screen_now_ms();
    if (remaining < 100 || (renew && screen_now_ms() >= leaseDeadline_)) return false;
    const auto command = renew ? "renew " + QByteArray::number(expires) : "watch " + QByteArray::number(expires) + " " + url.toUtf8() + " " + token.toUtf8() + " " + room.toUtf8() + " " + identity.toUtf8() + " gcp-screen-publish-" + selected_.toUtf8();
    if (!control(command)) return false;
    leaseDeadline_ = expires; deadline_.start(static_cast<int>(remaining)); renewal_.start(qMin(runtime_.renewMs, qMax(100, static_cast<int>(remaining / 3)))); return true;
}
void ScreenViewer::requestTrack() {
    if (!consent_ || stopping_ || pending_ || !connected_ || !sameScope()) return;
    if (account_->busy()) { renewal_.start(100); return; }
    QUrl url(origin_); url.setPath(path() + QStringLiteral("/%1/watch").arg(lease_)); QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + account_->accessToken().toUtf8()); request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(runtime_.requestMs);
    auto* reply = network_.post(request, QJsonDocument(QJsonObject{{"consent", true}, {"publisher_id", selected_}}).toJson(QJsonDocument::Compact));
    pending_ = reply; boundReply(reply, 16384, runtime_.requestMs); const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        if (pending_ != reply || generation != generation_) { reply->deleteLater(); return; }
        pending_ = nullptr; const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); const auto bytes = reply->readAll(); const auto json = QJsonDocument::fromJson(bytes).object(); const auto sid = json.value("track_sid").toString();
        const bool valid = reply->error() == QNetworkReply::NoError && code == 200 && sameScope() && bytes.size() <= 16384 && json.value("participant_identity").toString() == QStringLiteral("gcp-screen-publish-") + selected_ && gcp::transport::screen_track_sid(sid.toStdString()) && (trackSid_.isEmpty() || trackSid_ == sid);
        reply->deleteLater(); if (!valid) { stopWithReason(denied(code)); return; }
        if (trackSid_.isEmpty()) { trackSid_ = sid; if (!control("track " + sid.toUtf8())) stopWithReason(QStringLiteral("Viewing stopped: media process unavailable.")); }
    });
}
void ScreenViewer::requestFrame() {
    if (!consent_ || stopping_ || !subscribed_ || credit_) return;
    if (!control("next")) { stopWithReason(QStringLiteral("Viewing stopped: video control failed.")); return; }
    credit_ = true;
}
void ScreenViewer::readOutput() {
    if (stopping_) { process_.closeReadChannel(QProcess::StandardOutput); return; }
    qsizetype drained = 0;
    while (process_.bytesAvailable() > 0 && drained < 262144 && !stopping_) {
        if (header_) {
            const auto data = process_.read(qMin<qint64>(65536, static_cast<qint64>(header_->bytes) - pixels_.size())); drained += data.size(); if (data.isEmpty()) break; pixels_ += data;
            if (pixels_.size() != static_cast<qsizetype>(header_->bytes)) continue;
            QImage next(header_->width, header_->height, QImage::Format_RGBA8888);
            if (next.isNull() || next.sizeInBytes() != pixels_.size()) { stopWithReason(QStringLiteral("Viewing stopped: invalid video storage.")); return; }
            std::memcpy(next.bits(), pixels_.constData(), header_->bytes); image_ = std::move(next); rotation_ = header_->rotation; frameNumber_ = header_->sequence;
            pixels_.clear(); header_.reset(); payloadDeadline_.stop(); credit_ = false; watching_ = true; startup_.stop(); status_ = QStringLiteral("Watching %1").arg(publisherName_);
            emit frameChanged(); emit changed(); frameTimer_.start(runtime_.frameMs);
        } else {
            const auto data = process_.readLine(4097); if (data.isEmpty()) break; drained += data.size(); if (line_.isEmpty()) payloadDeadline_.start(5000); line_ += data;
            if (line_.size() > 4096) { stopWithReason(QStringLiteral("Viewing stopped: process output limit exceeded.")); return; }
            if (!line_.endsWith('\n')) continue;
            auto message = std::exchange(line_, {}); message.chop(1); if (message.endsWith('\r')) message.chop(1);
            if (!message.startsWith("GCP_VIEW ")) { stopWithReason(QStringLiteral("Viewing stopped: invalid media response.")); return; }
            message.remove(0, 9); payloadDeadline_.stop();
            if (message == "connected" && !connected_ && !lease_.isEmpty()) { connected_ = true; requestTrack(); }
            else if (message == "subscribed " + trackSid_.toUtf8() && !trackSid_.isEmpty() && !subscribed_) { subscribed_ = true; requestFrame(); }
            else if (message.startsWith("frame ") && subscribed_ && credit_) {
                header_ = gcp::transport::view_frame_header(message.toStdString());
                if (!header_ || header_->sequence != frameNumber_ + 1) { stopWithReason(QStringLiteral("Viewing stopped: invalid frame dimensions, size or sequence.")); return; }
                pixels_.reserve(static_cast<qsizetype>(header_->bytes)); payloadDeadline_.start(5000);
            } else { stopWithReason(QStringLiteral("Viewing stopped: share ended or media state changed. Refresh shares to retry.")); return; }
        }
    }
    if (process_.bytesAvailable() > 0 && !stopping_) { const auto generation = generation_; QTimer::singleShot(0, this, [this, generation] { if (generation == generation_) readOutput(); }); }
}
void ScreenViewer::revoke() {
    if (lease_.isEmpty()) return;
    QUrl url(origin_); url.setPath(path() + QStringLiteral("/") + std::exchange(lease_, {})); QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + access_.toUtf8()); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(3000);
    auto* reply = network_.deleteResource(request); cleanup_ = reply; boundReply(reply, 1024, 3000);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (cleanup_ == reply) cleanup_ = nullptr;
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 204) { stopReason_ += QStringLiteral(" Server cleanup unconfirmed; the lease expires within a minute."); if (process_.state() == QProcess::NotRunning) status_ = stopReason_; }
        reply->deleteLater(); emit changed();
    });
}
void ScreenViewer::stop() { stopWithReason(QStringLiteral("Viewing stopped")); }
void ScreenViewer::stopWithReason(const QString& reason) {
    if (stopping_) return;
    ++generation_; stopping_ = true; stopReason_ = reason; consent_ = watching_ = connected_ = subscribed_ = credit_ = false;
    for (auto* timer : {&heartbeat_, &renewal_, &deadline_, &startup_, &frameTimer_, &payloadDeadline_}) timer->stop();
    abort(pending_); abort(listing_); image_ = {}; pixels_.clear(); line_.clear(); header_.reset(); rotation_ = 0; frameNumber_ = 0; trackSid_.clear(); emit frameChanged(); revoke();
    access_.fill(QChar(0)); access_.clear();
    if (process_.state() != QProcess::NotRunning) { status_ = QStringLiteral("Stopping screen viewer…"); control("stop"); process_.closeWriteChannel(); process_.closeReadChannel(QProcess::StandardOutput); kill_.start(1500); }
    else status_ = reason;
    emit ended(); emit changed();
}
void ScreenViewer::processFinished() {
    if (!stopping_) stopWithReason(QStringLiteral("Viewing stopped: media process closed. Refresh shares to retry."));
    heartbeat_.stop(); kill_.stop(); status_ = stopReason_; emit changed();
}
