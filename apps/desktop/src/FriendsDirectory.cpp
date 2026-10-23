#include "FriendsDirectory.h"
#include "AccountSession.h"
#include "BoundedReply.h"
#include "ChatProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QSet>
#include <algorithm>
#include <utility>
namespace {
using ChatProtocol::uuid;
QVariantMap find(const QVariantList &list, const QString &id, const QString &key = QStringLiteral("user_id")) {
    for (const auto &item : list) if (item.toMap().value(key).toString() == id) return item.toMap();
    return {};
}
bool handleValid(const QString &value) {
    if (value.size() < 3 || value.size() > 32) return false;
    for (const auto c : value) if (!((c >= QChar('a') && c <= QChar('z')) || (c >= QChar('0') && c <= QChar('9')) || c == QChar('_'))) return false;
    return true;
}
QVariantMap profile(const QJsonObject &object, const QString &self) {
    const auto id = object.value(QStringLiteral("user_id")).toString();
    const auto handle = object.value(QStringLiteral("handle")).toString();
    const auto name = object.value(QStringLiteral("display_name")).toString();
    if (!uuid(id) || id == self || !handleValid(handle) || name.trimmed().isEmpty() || name.size() > 160) return {};
    return {{"user_id", id}, {"handle", handle}, {"display_name", name}};
}
}
FriendsDirectory::FriendsDirectory(AccountSession *account, QObject *parent, FriendsRuntime runtime)
    : QObject(parent), account_(account), runtime_(runtime) {
    runtime_.pollMs = qMax(100, runtime_.pollMs); runtime_.deadlineMs = qBound(100, runtime_.deadlineMs, 10000);
    network_.setProxy(QNetworkProxy::NoProxy); poll_.setInterval(runtime_.pollMs);
    connect(&poll_, &QTimer::timeout, this, [this]() { refreshWanted_ = true; refresh(); });
    connect(account_, &AccountSession::signedOut, this, &FriendsDirectory::reset);
    connect(account_, &AccountSession::changed, this, &FriendsDirectory::accountChanged);
    accountChanged();
}
FriendsDirectory::~FriendsDirectory() {
    abort();
    for (auto *reply : network_.findChildren<QNetworkReply *>()) { reply->disconnect(this); reply->abort(); }
}
void FriendsDirectory::abort() {
    ++version_;
    if (auto *old = std::exchange(pending_, nullptr)) { old->disconnect(this); old->abort(); old->deleteLater(); }
    processing_ = false; mutation_ = false;
    nextFriends_.clear(); nextRequests_.clear(); nextBlocks_.clear(); nextConversations_.clear();
}
void FriendsDirectory::reset() {
    abort(); poll_.stop(); verified_ = false; refreshWanted_ = false;
    friends_.clear(); requests_.clear(); blocks_.clear(); conversations_.clear();
    conversation_.clear(); peer_.clear(); token_.clear(); user_.clear(); outcome_.clear();
    status_ = QStringLiteral("Sign in to manage friends"); emit selectionChanged(); emit changed();
}
void FriendsDirectory::accountChanged() {
    if (!account_->signedIn()) { if (!user_.isEmpty() || busy()) reset(); return; }
    if (user_ != account_->userId()) { reset(); user_ = account_->userId(); refreshWanted_ = true; }
    if (token_ != account_->accessToken()) {
        token_ = account_->accessToken(); verified_ = false; refreshWanted_ = true;
        // Do not abort or replay an uncertain social mutation on token rotation.
        if (!mutation_) abort();
    }
    poll_.start();
    if (refreshWanted_ && !busy() && !account_->busy()) refresh();
    emit changed();
}
bool FriendsDirectory::ready() const { return account_->signedIn() && uuid(account_->userId()) && !account_->busy() && !busy(); }
QString FriendsDirectory::peerName() const {
    const auto row = find(friends_, peer_);
    return row.isEmpty() ? QStringLiteral("Private conversation") : row.value(QStringLiteral("display_name")).toString();
}
void FriendsDirectory::refresh() {
    if (!ready()) { refreshWanted_ = account_->signedIn(); return; }
    refreshWanted_ = false; verified_ = false;
    nextFriends_.clear(); nextRequests_.clear(); nextBlocks_.clear(); nextConversations_.clear();
    send(Operation::Friends, "GET", QStringLiteral("/v1/friends"));
}
void FriendsDirectory::cancel() {
    abort(); verified_ = false; refreshWanted_ = false;
    status_ = QStringLiteral("Cancelled. Refresh friends to confirm whether the change reached the server."); emit changed();
}
void FriendsDirectory::select(const QVariantMap &row) {
    const auto id = row.value(QStringLiteral("id")).toString();
    const auto peer = row.value(QStringLiteral("other_user_id")).toString();
    if (id == conversation_ && peer == peer_) return;
    conversation_ = id; peer_ = peer; emit selectionChanged(); emit changed();
}
void FriendsDirectory::closeConversation() { select({}); }
void FriendsDirectory::chooseConversation(const QString &id) {
    if (!ready() || !verified_) return;
    const auto row = find(conversations_, id, QStringLiteral("id"));
    if (!row.isEmpty() && !find(friends_, row.value(QStringLiteral("other_user_id")).toString()).isEmpty()) select(row);
}
void FriendsDirectory::revokePeer(const QString &peer) {
    emit relationshipLost(peer);
    for (qsizetype i = conversations_.size(); i > 0; --i)
        if (conversations_[i - 1].toMap().value(QStringLiteral("other_user_id")).toString() == peer) conversations_.removeAt(i - 1);
    if (peer_ == peer) closeConversation();
}
void FriendsDirectory::invalidateConversation(const QString &id) {
    if (id != conversation_ || id.isEmpty()) return;
    const auto peer = peer_; revokePeer(peer); verified_ = false;
    status_ = QStringLiteral("Private conversation access changed. Refresh friends or sign in again.");
    emit changed(); refreshWanted_ = true;
    if (ready()) refresh();
}
void FriendsDirectory::sendRequest(const QString &handle) {
    if (!ready() || !verified_) return;
    if (!handleValid(handle)) { status_ = QStringLiteral("Use an exact 3–32 character lowercase handle with digits or underscores."); emit changed(); return; }
    send(Operation::Send, "POST", QStringLiteral("/v1/friends/requests"), {{"handle", handle}});
}
void FriendsDirectory::resolveRequest(const QString &id, Operation operation) {
    if (!ready() || !verified_) return;
    const auto row = find(requests_, id, QStringLiteral("id"));
    if (row.isEmpty()) return;
    const bool incoming = row.value(QStringLiteral("receiver_id")).toString() == user_;
    if ((operation == Operation::Cancel) == incoming) return;
    const auto suffix = operation == Operation::Accept ? QStringLiteral("/accept") : operation == Operation::Decline ? QStringLiteral("/decline") : QString{};
    send(operation, operation == Operation::Cancel ? "DELETE" : "POST", QStringLiteral("/v1/friends/requests/") + id + suffix, {}, id);
}
void FriendsDirectory::acceptRequest(const QString &id) { resolveRequest(id, Operation::Accept); }
void FriendsDirectory::declineRequest(const QString &id) { resolveRequest(id, Operation::Decline); }
void FriendsDirectory::cancelRequest(const QString &id) { resolveRequest(id, Operation::Cancel); }
void FriendsDirectory::removeFriend(const QString &id) {
    if (ready() && verified_ && !find(friends_, id).isEmpty()) send(Operation::Remove, "DELETE", QStringLiteral("/v1/friends/") + id, {}, id);
}
void FriendsDirectory::blockUser(const QString &id) {
    if (!ready() || !verified_ || !uuid(id) || id == user_) return;
    bool known = !find(friends_, id).isEmpty();
    for (const auto &row : requests_) if (row.toMap().value(QStringLiteral("user_id")).toString() == id) known = true;
    if (known) send(Operation::Block, "POST", QStringLiteral("/v1/blocks"), {{"user_id", id}}, id);
}
void FriendsDirectory::unblockUser(const QString &id) {
    if (ready() && verified_ && !find(blocks_, id).isEmpty()) send(Operation::Unblock, "DELETE", QStringLiteral("/v1/blocks/") + id, {}, id);
}
void FriendsDirectory::openConversation(const QString &id) {
    if (ready() && verified_ && !find(friends_, id).isEmpty()) send(Operation::Open, "POST", QStringLiteral("/v1/dms"), {{"user_id", id}}, id);
}
void FriendsDirectory::send(Operation operation, const QByteArray &method, const QString &path, const QJsonObject &body, const QString &target) {
    QUrl url = account_->apiOrigin();
    if (!ChatProtocol::originValid(url)) { verified_ = false; status_ = QStringLiteral("Use a valid HTTPS server origin."); emit changed(); return; }
    const bool read = operation == Operation::Friends || operation == Operation::Requests || operation == Operation::Blocks || operation == Operation::Conversations;
    mutation_ = !read; verified_ = false; processing_ = false;
    url.setPath(path); QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + account_->accessToken().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json")); request.setTransferTimeout(runtime_.deadlineMs);
    auto payload = body.isEmpty() ? QByteArray{} : QJsonDocument(body).toJson(QJsonDocument::Compact);
    pending_ = network_.sendCustomRequest(request, method, payload); payload.fill(0);
    boundReply(pending_, 262144, runtime_.deadlineMs);
    status_ = read ? QStringLiteral("Refreshing friends…") : QStringLiteral("Saving friend action…");
    auto *reply = pending_; const auto version = version_; const auto access = account_->accessToken();
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, access, operation, target, read]() {
        if (pending_ != reply || version != version_) { reply->deleteLater(); return; }
        pending_ = nullptr; processing_ = true;
        const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error(); auto bytes = reply->readAll(); reply->deleteLater();
        const int expected = read || operation == Operation::Open ? 200 : operation == Operation::Send ? 201 : 204;
        const bool valid = error == QNetworkReply::NoError && http == expected && apply(operation, bytes, target);
        bytes.fill(0);
        if (!valid) {
            processing_ = false; mutation_ = false;
            if (http == 401 && (account_->busy() || account_->accessToken() != access)) {
                refreshWanted_ = true;
                status_ = QStringLiteral("Waiting for the refreshed session; no action was replayed.");
            } else failed(http, error);
        }
        emit changed();
        if (refreshWanted_ && ready()) refresh();
    });
    emit changed();
}
bool FriendsDirectory::apply(Operation operation, const QByteArray &bytes, const QString &target) {
    if (operation == Operation::Friends || operation == Operation::Requests || operation == Operation::Blocks || operation == Operation::Conversations) {
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
        const int limit = operation == Operation::Requests || operation == Operation::Conversations ? 100 : 200;
        if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > limit) return false;
        QVariantList parsed; QSet<QString> seen, peers;
        for (const auto &value : doc.array()) {
            if (!value.isObject()) return false;
            const auto object = value.toObject(); QVariantMap row; QString key;
            if (operation == Operation::Conversations) {
                key = object.value(QStringLiteral("id")).toString(); const auto peer = object.value(QStringLiteral("other_user_id")).toString();
                if (!uuid(key) || !uuid(peer) || peer == user_ || peers.contains(peer)) return false;
                peers.insert(peer); row = {{"id", key}, {"other_user_id", peer}};
            } else {
                row = profile(object, user_); if (row.isEmpty()) return false;
                key = row.value(QStringLiteral("user_id")).toString();
                if (operation == Operation::Requests) {
                    key = object.value(QStringLiteral("id")).toString();
                    const auto sender = object.value(QStringLiteral("sender_id")).toString(), receiver = object.value(QStringLiteral("receiver_id")).toString();
                    if (!uuid(key) || !uuid(sender) || !uuid(receiver) || sender == receiver ||
                        !((sender == user_ && receiver == row.value("user_id").toString()) || (receiver == user_ && sender == row.value("user_id").toString()))) return false;
                    row.insert(QStringLiteral("id"), key); row.insert(QStringLiteral("sender_id"), sender); row.insert(QStringLiteral("receiver_id"), receiver);
                }
            }
            if (seen.contains(key)) return false; seen.insert(key); parsed.append(row);
        }
        switch (operation) {
        case Operation::Friends: nextFriends_ = parsed; send(Operation::Requests, "GET", QStringLiteral("/v1/friends/requests")); break;
        case Operation::Requests: nextRequests_ = parsed; send(Operation::Blocks, "GET", QStringLiteral("/v1/blocks")); break;
        case Operation::Blocks: nextBlocks_ = parsed; send(Operation::Conversations, "GET", QStringLiteral("/v1/dms")); break;
        case Operation::Conversations: nextConversations_ = parsed; publish(); break;
        default: return false;
        }
        return true;
    }
    if (operation == Operation::Open) {
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error); const auto object = doc.object();
        const auto id = object.value(QStringLiteral("id")).toString(), peer = object.value(QStringLiteral("other_user_id")).toString();
        if (error.error != QJsonParseError::NoError || !doc.isObject() || !uuid(id) || peer != target) return false;
        const QVariantMap row{{"id", id}, {"other_user_id", peer}};
        select(row); outcome_ = QStringLiteral("Private conversation opened.");
    } else if (operation == Operation::Remove || operation == Operation::Block) {
        revokePeer(target); outcome_ = operation == Operation::Block ? QStringLiteral("User blocked. Friendship and pending requests were removed.") : QStringLiteral("Friend removed. Private messaging is unavailable.");
    } else outcome_ = operation == Operation::Unblock ? QStringLiteral("User unblocked. Send a new friend request to restore friendship.") : QStringLiteral("Friend action saved.");
    mutation_ = false; processing_ = false; refreshWanted_ = true; return true;
}
void FriendsDirectory::publish() {
    const auto oldFriends = friends_;
    friends_ = std::move(nextFriends_); requests_ = std::move(nextRequests_); blocks_ = std::move(nextBlocks_);
    const auto selected = QVariantMap{{"id", conversation_}, {"other_user_id", peer_}};
    conversations_ = std::move(nextConversations_);
    for (const auto &row : oldFriends) {
        const auto peer = row.toMap().value(QStringLiteral("user_id")).toString();
        if (!find(blocks_, peer).isEmpty() || (friends_.size() < 200 && find(friends_, peer).isEmpty())) revokePeer(peer);
    }
    if (!conversation_.isEmpty()) {
        if (!find(blocks_, peer_).isEmpty() || (friends_.size() < 200 && find(friends_, peer_).isEmpty())) revokePeer(peer_);
        else if (find(conversations_, conversation_, QStringLiteral("id")).isEmpty()) conversations_.prepend(selected);
    }
    while (conversations_.size() > 101) conversations_.removeLast();
    for (auto &item : conversations_) {
        auto row = item.toMap(); const auto person = find(friends_, row.value(QStringLiteral("other_user_id")).toString());
        row.insert(QStringLiteral("display_name"), person.value(QStringLiteral("display_name"), QStringLiteral("Private conversation")));
        row.insert(QStringLiteral("handle"), person.value(QStringLiteral("handle"))); item = row;
    }
    verified_ = true; processing_ = false; mutation_ = false;
    status_ = outcome_.isEmpty() ? QStringLiteral("Friends up to date. Lists refresh every 30 seconds; Refresh checks now.") : outcome_; outcome_.clear();
}
void FriendsDirectory::failed(int http, QNetworkReply::NetworkError error) {
    verified_ = false;
    if (http == 401) { reset(); status_ = QStringLiteral("Session expired. Sign in again to manage friends."); }
    else if (http == 404) status_ = QStringLiteral("User, request or private conversation is unavailable. Refresh friends before trying again.");
    else if (http == 409) status_ = QStringLiteral("Already friends, a request is pending, or requests are unavailable. Refresh to check.");
    else if (http == 400 || http == 422) status_ = QStringLiteral("Check the exact handle or selected friend action.");
    else if (http == 503) status_ = QStringLiteral("Server is not ready. Your action was not confirmed; refresh before retrying.");
    else if (http >= 300 && http < 400) status_ = QStringLiteral("Redirect blocked. Use the final HTTPS server address.");
    else if (error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError) status_ = QStringLiteral("Request timed out or exceeded its limit. Refresh to confirm any uncertain action.");
    else status_ = QStringLiteral("Could not verify the friend action. Refresh before trying again.");
    nextFriends_.clear(); nextRequests_.clear(); nextBlocks_.clear(); nextConversations_.clear();
}
