#include "ChatSession.h"
#include "AccountSession.h"
#include "BoundedReply.h"
#include "ChannelDirectory.h"
#include "FriendsDirectory.h"
#include "ChatProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int pageSize = 50;
constexpr int maximumEvents = 256;
constexpr int maximumDrafts = 10;
using ChatProtocol::uuid;
using ChatProtocol::bodyValid;
using ChatProtocol::originValid;
std::optional<qint64> timestamp(const QJsonValue &value, bool nullable) {
    if (nullable && value.isNull()) return 0;
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) return std::nullopt;
    return static_cast<qint64>(number);
}
}

ChatSession::ChatSession(AccountSession *account, ChannelDirectory *directory, QObject *parent, ChatRuntime runtime)
    : ChatSession(account, directory, nullptr, parent, runtime) {}
ChatSession::ChatSession(AccountSession *account, FriendsDirectory *friends, QObject *parent, ChatRuntime runtime)
    : ChatSession(account, nullptr, friends, parent, runtime) {}
ChatSession::ChatSession(AccountSession *account, ChannelDirectory *directory, FriendsDirectory *friends, QObject *parent, ChatRuntime runtime)
    : QObject(parent), account_(account), directory_(directory), friends_(friends), runtime_(runtime) {
    runtime_.retryBaseMs = qBound(10, runtime_.retryBaseMs, 30000);
    runtime_.pollMs = qMax(100, runtime_.pollMs);
    runtime_.deadlineMs = qBound(100, runtime_.deadlineMs, 10000);
    network_.setProxy(QNetworkProxy::NoProxy);
    for (auto *timer : {&pumpTimer_, &retryTimer_, &socketDeadline_, &accountDeadline_}) timer->setSingleShot(true);
    pumpTimer_.setInterval(50);
    connect(&pumpTimer_, &QTimer::timeout, this, &ChatSession::pump);
    connect(&retryTimer_, &QTimer::timeout, this, &ChatSession::openSocket);
    connect(&socketDeadline_, &QTimer::timeout, this, &ChatSession::socketFailed);
    connect(&accountDeadline_, &QTimer::timeout, this, [this]() {
        awaitingAccount_ = false;
        accessLost(); setStatus(QStringLiteral("Session refresh timed out; sign in or refresh messages."));
    });
    pollTimer_.setInterval(runtime_.pollMs);
    connect(&pollTimer_, &QTimer::timeout, this, [this]() {
        if (!socketReady_ && !blocked_) { wantLatest_ = true; pump(); }
    });
    heartbeatTimer_.setInterval(20000);
    connect(&heartbeatTimer_, &QTimer::timeout, this, [this]() {
        if (socketReady_ && socket_) { socketDeadline_.start(runtime_.deadlineMs); socket_->ping(); }
    });
    if (directory_) {
        connect(directory_, &ChannelDirectory::textSelectionChanged, this, &ChatSession::selection);
        connect(directory_, &ChannelDirectory::changed, this, [this]() { selection(); emit changed(); });
    } else {
        connect(friends_, &FriendsDirectory::selectionChanged, this, &ChatSession::selection);
        connect(friends_, &FriendsDirectory::changed, this, [this]() { selection(); emit changed(); });
        connect(friends_, &FriendsDirectory::relationshipLost, this, [this](const QString &peer) {
            for (auto it = drafts_.begin(); it != drafts_.end(); ) {
                if (it.value().peer == peer) it = drafts_.erase(it); else ++it;
            }
            emit draftChanged(); emit changed();
        });
    }
    connect(account_, &AccountSession::changed, this, &ChatSession::accountChanged);
    connect(account_, &AccountSession::signedOut, this, [this]() {
        drafts_.clear(); selection(); emit draftChanged();
    });
    status_ = choiceStatus(); accountChanged(); selection();
}
ChatSession::~ChatSession() {
    stopTransport();
    abortReply(mutation_);
    for (auto *reply : network_.findChildren<QNetworkReply *>()) { reply->disconnect(this); reply->abort(); }
}
void ChatSession::abortReply(QNetworkReply *&reply) {
    if (!reply) return;
    auto *old = std::exchange(reply, nullptr);
    old->disconnect(this); old->abort(); old->deleteLater();
}
void ChatSession::stopSocket() {
    socketDeadline_.stop(); heartbeatTimer_.stop(); socketReady_ = false;
    if (!socket_) return;
    auto *old = socket_.release();
    old->disconnect(this); old->abort(); old->deleteLater(); // parent owns deferred destruction too
}
void ChatSession::stopTransport() {
    pumpTimer_.stop(); retryTimer_.stop(); pollTimer_.stop(); accountDeadline_.stop();
    awaitingAccount_ = false; abortReply(read_); stopSocket(); events_.clear();
}
QString ChatSession::path() const {
    return friends_ ? QStringLiteral("/v1/dms/") + channel_ : QStringLiteral("/v1/communities/%1/channels/%2").arg(community_, channel_);
}
QString ChatSession::scopeField() const { return friends_ ? QStringLiteral("conversation_id") : QStringLiteral("channel_id"); }
QString ChatSession::choiceStatus() const { return friends_ ? QStringLiteral("Choose a private conversation") : QStringLiteral("Choose a text channel"); }
QString ChatSession::title() const {
    if (friends_) return friends_->conversationId().isEmpty() ? QStringLiteral("Direct messages") : friends_->peerName();
    return directory_->textChannelName().isEmpty() ? QStringLiteral("Text messages") : QStringLiteral("# ") + directory_->textChannelName();
}
bool ChatSession::canModerate() const { return directory_ && directory_->canManage(); }
void ChatSession::directAuthors() {
    if (!friends_) return;
    names_.clear(); names_.insert(account_->userId(), account_->displayName());
    if (!friends_->peerId().isEmpty()) names_.insert(friends_->peerId(), friends_->peerName());
    model_.authors(names_);
}
QString ChatSession::draftKey() const { return community_ + QChar('/') + channel_; }
QString ChatSession::draft() const { return drafts_.value(draftKey()).body; }
bool ChatSession::retrySend() const { return !drafts_.value(draftKey()).attempt.isEmpty(); }
bool ChatSession::canMutate() const {
    return authorized_ && !blocked_ && !channel_.isEmpty() && account_->signedIn() && uuid(account_->userId()) && !account_->busy() && (friends_ ? friends_->verified() && !friends_->mutating() : !directory_->busy()) && !sending();
}
bool ChatSession::canSubmit() const { return canMutate() && bodyValid(draft()); }
void ChatSession::setStatus(const QString &status) { status_ = status; emit changed(); }
void ChatSession::selection() {
    const auto community = account_->signedIn() ? (friends_ ? QStringLiteral("dm") : directory_->communityId()) : QString{};
    const auto channel = account_->signedIn() ? (friends_ ? friends_->conversationId() : directory_->textChannelId()) : QString{};
    if (community == community_ && channel == channel_) { directAuthors(); return; }
    if (!friends_ && !community_.isEmpty() && community.isEmpty()) {
        for (auto it = drafts_.begin(); it != drafts_.end(); ) {
            if (it.key().startsWith(community_ + QChar('/'))) it = drafts_.erase(it); else ++it;
        }
    } else if (!friends_ && community == community_ && channel.isEmpty()) {
        drafts_.remove(draftKey()); // The selected text channel was removed.
    }
    ++generation_; stopTransport(); abortReply(mutation_);
    community_ = community; channel_ = channel;
    authorized_ = false; blocked_ = false; before_.clear(); hasOlder_ = false;
    followingLatest_ = true; newerAvailable_ = false; failures_ = 0;
    wantLatest_ = !channel_.isEmpty(); wantMembers_ = wantLatest_ && directory_; wantOlder_ = false;
    names_.clear(); model_.authors(names_); model_.replace({}); directAuthors();
    emit scopeChanged(); emit draftChanged();
    if ((!friends_ && !uuid(community_)) || !uuid(channel_)) { setStatus(choiceStatus()); return; }
    setStatus(QStringLiteral("Loading messages…"));
    pollTimer_.start(); openSocket(); pump();
}
void ChatSession::accountChanged() {
    if (!account_->signedIn()) {
        drafts_.clear(); token_.clear(); selection(); emit draftChanged(); emit changed(); return;
    }
    if (token_ != account_->accessToken()) {
        token_ = account_->accessToken();
        // A login rotation must replace the socket's old Authorization header.
        // Pending writes retain their IDs and are never automatically repeated.
        stopTransport(); authorized_ = false; blocked_ = false;
        wantLatest_ = !channel_.isEmpty(); wantMembers_ = wantLatest_ && directory_; wantOlder_ = false;
        if (!channel_.isEmpty()) { pollTimer_.start(); openSocket(); pump(); }
    } else if (awaitingAccount_ && !account_->busy()) {
        accountDeadline_.stop(); awaitingAccount_ = false;
        wantLatest_ = true; pump();
    }
    emit changed();
}
bool ChatSession::setDraft(const QString &body) {
    if (channel_.isEmpty() || !account_->signedIn() || body.size() > 4000 || sending()) return false;
    const auto key = draftKey();
    if (drafts_.value(key).body == body) return true;
    if (retrySend()) { setStatus(QStringLiteral("Retry this send or clear the draft before editing.")); return false; }
    if (!body.isEmpty() && !drafts_.contains(key) && drafts_.size() >= maximumDrafts) {
        setStatus(QStringLiteral("Ten chats already have drafts. Clear one before composing here.")); return false;
    }
    if (body.isEmpty()) drafts_.remove(key); else {
        drafts_[key].body = body;
        if (friends_) drafts_[key].peer = friends_->peerId();
    }
    emit draftChanged(); emit changed(); return true;
}
void ChatSession::clearDraft() {
    if (sending()) return;
    const bool pending = retrySend(); drafts_.remove(draftKey()); emit draftChanged(); emit changed();
    if (pending) setStatus(QStringLiteral("Retry cancelled. Check history: the message may already have been delivered."));
}
void ChatSession::sendDraft() {
    if (!canSubmit()) return;
    auto &entry = drafts_[draftKey()];
    if (entry.attempt.isEmpty()) entry.attempt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!followingLatest_) { wantLatest_ = true; wantOlder_ = false; }
    followingLatest_ = true;
    mutate(Mutation::Create, entry.attempt, entry.body);
    emit draftChanged();
}
void ChatSession::editMessage(const QString &id, const QString &body) {
    const auto row = model_.find(id);
    if (!canMutate() || !row || row->deletedMs || row->author != account_->userId()) return;
    if (!bodyValid(body)) { setStatus(QStringLiteral("Messages need 1–4000 UTF-8 bytes without control characters.")); return; }
    mutate(Mutation::Edit, id, body);
}
void ChatSession::deleteMessage(const QString &id) {
    const auto row = model_.find(id);
    if (!canMutate() || !row || row->deletedMs || (row->author != account_->userId() && !canModerate())) return;
    mutate(Mutation::Delete, id);
}
void ChatSession::refreshLatest() {
    if (channel_.isEmpty() || !account_->signedIn()) return;
    blocked_ = false; wantLatest_ = true; wantMembers_ = directory_ != nullptr; wantOlder_ = false;
    pollTimer_.start();
    if (!socket_) openSocket();
    pump();
}
void ChatSession::loadOlder() {
    if (!authorized_ || !hasOlder_ || read_ || wantLatest_ || before_.isEmpty()) return;
    wantOlder_ = true; followingLatest_ = false; pump();
}
void ChatSession::reconnect() {
    if (channel_.isEmpty()) return;
    blocked_ = false; stopSocket(); retryTimer_.stop(); failures_ = 0;
    pollTimer_.start();
    wantLatest_ = true; wantMembers_ = directory_ != nullptr; openSocket(); pump();
}
void ChatSession::openSocket() {
    if (channel_.isEmpty() || blocked_ || socket_ || !account_->signedIn() || awaitingAccount_) return;
    if (account_->busy()) { retryTimer_.start(runtime_.retryBaseMs); return; }
    QUrl url = account_->apiOrigin();
    if (!originValid(url)) { accessLost(); setStatus(QStringLiteral("Use a valid HTTPS server origin.")); return; }
    url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    url.setPath(path() + QStringLiteral("/events"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    socket_ = std::make_unique<QWebSocket>(QString{}, QWebSocketProtocol::VersionLatest, this);
    socket_->setProxy(QNetworkProxy::NoProxy);
    socket_->setReadBufferSize(16384);
    socket_->setMaxAllowedIncomingFrameSize(2048);
    socket_->setMaxAllowedIncomingMessageSize(2048);
    auto *socket = socket_.get();
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &text) { if (socket_.get() == socket) socketEvent(text); });
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this]() { socketFailed(); });
    connect(socket, &QWebSocket::disconnected, this, &ChatSession::socketFailed);
    connect(socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { socketFailed(); });
    connect(socket, &QWebSocket::pong, this, [this](quint64, const QByteArray &) { if (socketReady_) socketDeadline_.stop(); });
    socketDeadline_.start(runtime_.deadlineMs); socket_->open(request);
}
void ChatSession::socketFailed() {
    stopSocket();
    if (channel_.isEmpty() || blocked_ || !account_->signedIn()) return;
    if (!retryTimer_.isActive()) {
        const int delay = qMin(30000, runtime_.retryBaseMs * (1 << qMin(failures_, 5)));
        failures_ = qMin(failures_ + 1, 6); retryTimer_.start(delay);
    }
    wantLatest_ = true; pumpTimer_.start();
    setStatus(QStringLiteral("Live updates interrupted; reconnecting and checking history…"));
}
void ChatSession::socketEvent(const QString &text) {
    if (text.toUtf8().size() > 2048) { socketFailed(); return; }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { socketFailed(); return; }
    const auto object = document.object();
    const auto type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("resync_required")) { socketFailed(); return; }
    if (object.value(scopeField()).toString() != channel_) { socketFailed(); return; }
    if (type == QStringLiteral("ready")) {
        if (socketReady_) { socketFailed(); return; }
        socketReady_ = true; failures_ = 0; socketDeadline_.stop(); heartbeatTimer_.start();
        // Subscribe before the authoritative snapshot, then reconcile queued IDs.
        wantLatest_ = true; wantMembers_ = directory_ != nullptr; pump(); emit changed(); return;
    }
    if (!socketReady_) { socketFailed(); return; }
    const auto user = object.value(QStringLiteral("user_id")).toString();
    if (!uuid(user) || (friends_ && user != account_->userId() && user != friends_->peerId())) { socketFailed(); return; }
    if (type == QStringLiteral("message_created") || type == QStringLiteral("message_edited") || type == QStringLiteral("message_deleted")) {
        const auto id = object.value(QStringLiteral("message_id")).toString();
        if (!uuid(id)) { socketFailed(); return; }
        if (followingLatest_ || model_.find(id) || drafts_.value(draftKey()).attempt == id) queueMessage(id);
        else { newerAvailable_ = true; emit changed(); }
        if (!names_.contains(user)) wantMembers_ = directory_ != nullptr;
    } else if (type == QStringLiteral("presence_joined") && !names_.contains(user)) {
        wantMembers_ = directory_ != nullptr; pumpTimer_.start();
    } // Typing/presence are supported by the API, not displayed as chat messages.
}
void ChatSession::queueMessage(const QString &id) {
    if (events_.size() >= maximumEvents && !events_.contains(id)) {
        events_.clear(); wantLatest_ = true;
    } else events_.insert(id);
    if (!pumpTimer_.isActive()) pumpTimer_.start();
}
void ChatSession::pump() {
    if (read_ || blocked_ || awaitingAccount_ || channel_.isEmpty() || !account_->signedIn() || account_->busy()) return;
    if (wantMembers_) { wantMembers_ = false; read(Read::Members); }
    else if (wantLatest_) { wantLatest_ = false; wantOlder_ = false; read(Read::Latest); }
    else if (wantOlder_) { wantOlder_ = false; read(Read::Older, before_); }
    else if (!events_.isEmpty()) { const auto id = *events_.cbegin(); events_.remove(id); read(Read::Message, id); }
}
void ChatSession::read(Read operation, const QString &id) {
    QUrl url = account_->apiOrigin();
    if (!originValid(url)) { accessLost(); return; }
    url.setPath(operation == Read::Members ? QStringLiteral("/v1/communities/") + community_ + QStringLiteral("/members") :
        path() + QStringLiteral("/messages") + (operation == Read::Message ? QChar('/') + id : QString{}));
    if (operation == Read::Latest || operation == Read::Older) {
        QUrlQuery query; query.addQueryItem(QStringLiteral("limit"), QString::number(pageSize));
        if (operation == Read::Older) query.addQueryItem(QStringLiteral("before"), id);
        url.setQuery(query);
    }
    QNetworkRequest request(url);
    const auto access = account_->accessToken();
    request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(runtime_.deadlineMs);
    read_ = network_.get(request);
    boundReply(read_, operation == Read::Message ? 16384 : 524288, runtime_.deadlineMs);
    auto *reply = read_; const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, id, generation, access]() {
        if (read_ != reply || generation != generation_) { reply->deleteLater(); return; }
        read_ = nullptr;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error(); auto bytes = reply->readAll(); reply->deleteLater();
        if (http == 401 && account_->signedIn() && (account_->busy() || account_->accessToken() != access)) {
            awaitingAccount_ = account_->busy(); accountDeadline_.start(runtime_.deadlineMs);
            wantLatest_ = true; wantMembers_ = directory_ != nullptr;
            bytes.fill(0); emit changed();
            if (!awaitingAccount_) { accountDeadline_.stop(); pumpTimer_.start(); }
            return;
        }
        if (http == 401 || http == 403 || (http == 404 && operation != Read::Message)) {
            bytes.fill(0); accessLost(); return;
        }
        bool valid = error == QNetworkReply::NoError && http == 200;
        if (valid && operation == Read::Members) valid = applyMembers(bytes);
        else if (valid && (operation == Read::Latest || operation == Read::Older)) valid = applyPage(bytes, operation == Read::Older, id);
        else if (valid) {
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(bytes, &parse);
            const auto row = parseMessage(doc.object());
            valid = parse.error == QJsonParseError::NoError && doc.isObject() && row && row->id == id;
            if (valid) merge(*row);
        }
        bytes.fill(0);
        if (!valid) {
            if (http == 404 && operation == Read::Message) { wantLatest_ = true; }
            else {
                if (operation == Read::Latest) authorized_ = false;
                setStatus(QStringLiteral("Could not verify messages. Your draft is kept; refresh to retry."));
            }
        } else if (operation == Read::Latest) {
            setStatus(socketReady_ ? QStringLiteral("Messages up to date · live") : QStringLiteral("History loaded · live updates reconnecting"));
        }
        emit changed(); pumpTimer_.start();
    });
    emit changed();
}
std::optional<MessageRow> ChatSession::parseMessage(const QJsonObject &object) const {
    MessageRow row;
    row.id = object.value(QStringLiteral("id")).toString(); row.channel = object.value(scopeField()).toString();
    row.author = object.value(QStringLiteral("author_id")).toString();
    const auto created = timestamp(object.value(QStringLiteral("created_at_us")), false);
    const auto millis = timestamp(object.value(QStringLiteral("created_at_ms")), false);
    const auto edited = timestamp(object.value(QStringLiteral("edited_at_ms")), true);
    const auto deleted = timestamp(object.value(QStringLiteral("deleted_at_ms")), true);
    if (!uuid(row.id) || row.channel != channel_ || !uuid(row.author) ||
        (friends_ && row.author != account_->userId() && row.author != friends_->peerId()) || !created || !millis || !edited || !deleted ||
        *created / 1000 != *millis || (*edited && *edited < *millis) || (*deleted && *deleted < *millis)) return std::nullopt;
    row.createdUs = *created; row.editedMs = *edited; row.deletedMs = *deleted;
    const auto body = object.value(QStringLiteral("body"));
    if (row.deletedMs) { if (!body.isNull()) return std::nullopt; }
    else { if (!body.isString() || !bodyValid(body.toString())) return std::nullopt; row.body = body.toString(); }
    return row;
}
bool ChatSession::applyMembers(const QByteArray &bytes) {
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 200) return false;
    QHash<QString, QString> names;
    for (const auto &value : doc.array()) {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("user_id")).toString();
        const auto name = object.value(QStringLiteral("display_name")).toString();
        if (!uuid(id) || name.trimmed().isEmpty() || name.size() > 160 || names.contains(id)) return false;
        names.insert(id, name);
    }
    names_ = names; model_.authors(names_); return true;
}
bool ChatSession::applyPage(const QByteArray &bytes, bool older, const QString &cursor) {
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const auto list = doc.object().value(QStringLiteral("messages"));
    const auto next = doc.object().value(QStringLiteral("next_before"));
    if (!list.isArray() || list.toArray().size() > pageSize || (!next.isNull() && !uuid(next.toString()))) return false;
    QVector<MessageRow> parsed; QSet<QString> seen;
    const auto boundary = older ? model_.find(cursor) : std::optional<MessageRow>{};
    if (older && !boundary) return false;
    for (const auto &value : list.toArray()) {
        if (!value.isObject()) return false;
        const auto row = parseMessage(value.toObject());
        if (!row || seen.contains(row->id) || (!parsed.isEmpty() && !MessageModel::before(*row, parsed.last())) ||
            (older && !MessageModel::before(*row, *boundary))) return false;
        seen.insert(row->id); parsed.append(*row);
    }
    if (!next.isNull() && (parsed.isEmpty() || next.toString() != parsed.last().id || next.toString() == cursor)) return false;
    for (const auto &row : parsed) resolveAttempt(row);
    if (older) {
        auto combined = model_.rows();
        for (const auto &row : parsed) if (!model_.find(row.id)) combined.append(row);
        std::sort(combined.begin(), combined.end(), MessageModel::before);
        while (combined.size() > MessageModel::maximumRows) combined.removeLast();
        model_.replace(std::move(combined));
    } else {
        model_.replace(std::move(parsed)); followingLatest_ = true; newerAvailable_ = false; authorized_ = true;
        // An uncertain POST may never have reached the server. Do not probe
        // its missing ID here: a 404 would request another snapshot forever.
        // Recent history/events can confirm it; explicit Retry safely resolves
        // an older or still-unconfirmed send using the same client UUID.
        emit historyReset();
    }
    before_ = next.toString(); hasOlder_ = !next.isNull();
    return true;
}
void ChatSession::resolveAttempt(const MessageRow &row) {
    if (row.author == account_->userId() && drafts_.value(draftKey()).attempt == row.id) {
        drafts_.remove(draftKey()); emit draftChanged(); emit changed();
    }
}
void ChatSession::merge(const MessageRow &row) {
    resolveAttempt(row);
    const bool full = model_.rowCount() >= MessageModel::maximumRows;
    model_.upsert(row, followingLatest_ || model_.find(row.id).has_value(), followingLatest_);
    if (followingLatest_ && full) {
        const auto rows = model_.rows();
        if (!rows.isEmpty()) { before_ = rows.first().id; hasOlder_ = true; }
    }
}
void ChatSession::mutate(Mutation operation, const QString &id, const QString &body) {
    if (!canMutate()) return;
    QUrl url = account_->apiOrigin();
    if (!originValid(url)) { accessLost(); return; }
    url.setPath(path() + QStringLiteral("/messages") + (operation == Mutation::Create ? QString{} : QChar('/') + id));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + account_->accessToken().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(runtime_.deadlineMs);
    QJsonObject object{{"body", body}};
    if (operation == Mutation::Create) object.insert(QStringLiteral("client_message_id"), id);
    auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    mutation_ = network_.sendCustomRequest(request, operation == Mutation::Create ? "POST" : operation == Mutation::Edit ? "PATCH" : "DELETE",
                                           operation == Mutation::Delete ? QByteArray{} : payload);
    payload.fill(0); boundReply(mutation_, 16384, runtime_.deadlineMs);
    auto *reply = mutation_; const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, id, generation]() {
        if (mutation_ != reply || generation != generation_) { reply->deleteLater(); return; }
        mutation_ = nullptr;
        const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error(); auto bytes = reply->readAll(); reply->deleteLater();
        bool valid = error == QNetworkReply::NoError && (operation == Mutation::Delete ? http == 204 : http == 200 || http == 201);
        if (valid && operation != Mutation::Delete) {
            QJsonParseError parse; const auto doc = QJsonDocument::fromJson(bytes, &parse);
            const auto row = parseMessage(doc.object());
            valid = parse.error == QJsonParseError::NoError && doc.isObject() && row && row->id == id && row->author == account_->userId();
            if (valid) merge(*row);
        }
        bytes.fill(0);
        if (valid) { queueMessage(id); setStatus(QStringLiteral("Change saved.")); }
        else if ((http == 401 && !account_->busy()) || http == 403 || http == 404) {
            // Mutation targets and access can disappear. Revalidate history;
            // do not repeat the write or discard an unconfirmed draft.
            wantLatest_ = true; setStatus(QStringLiteral("Access or message changed. Refreshing history before another change."));
        } else {
            setStatus(operation == Mutation::Create ? QStringLiteral("Delivery not confirmed. Retry sends this message safely once; your draft is kept.") :
                QStringLiteral("Change not confirmed. Refresh messages before trying again."));
        }
        emit mutationFinished(id, valid);
        emit draftChanged(); emit changed(); pumpTimer_.start();
    });
    emit changed();
}
void ChatSession::accessLost() {
    stopTransport(); abortReply(mutation_); authorized_ = false; blocked_ = true;
    wantLatest_ = false; wantMembers_ = false; wantOlder_ = false; hasOlder_ = false;
    if (friends_) drafts_.remove(draftKey());
    else for (auto it = drafts_.begin(); it != drafts_.end(); ) {
        if (it.key().startsWith(community_ + QChar('/'))) it = drafts_.erase(it); else ++it;
    }
    model_.replace({}); names_.clear(); model_.authors(names_);
    emit accessRevoked();
    emit draftChanged(); setStatus(QStringLiteral("Channel access expired or was removed. Refresh communities or sign in again."));
    if (friends_) friends_->invalidateConversation(channel_);
    else if (!directory_->busy() && account_->signedIn() && !account_->busy()) directory_->refresh();
}
