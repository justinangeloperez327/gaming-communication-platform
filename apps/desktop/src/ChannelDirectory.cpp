#include "ChannelDirectory.h"
#include "AccountSession.h"
#include "BoundedReply.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QSet>
#include <QUuid>
#include <QUrl>
#include <QVariantMap>

namespace {
constexpr int replyLimit = 131072;
bool validId(const QString &id) { return !QUuid(id).isNull(); }
bool validRole(const QString &role) {
    return role == QStringLiteral("owner") || role == QStringLiteral("admin") || role == QStringLiteral("member");
}
bool printableName(const QString &value, int limit) {
    if (value.isEmpty() || value != value.trimmed() || value.toUcs4().size() > limit) return false;
    for (const auto c : value)
        if (c.unicode() < 32 || (c.unicode() >= 127 && c.unicode() <= 159)) return false;
    return true;
}
bool validChannelName(const QString &value) {
    if (value.size() < 2 || value.size() > 80) return false;
    for (const auto c : value)
        if (!((c >= QChar('a') && c <= QChar('z')) || (c >= QChar('0') && c <= QChar('9')) ||
              c == QChar('-') || c == QChar('_'))) return false;
    return true;
}
bool hexToken(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QChar('0') && c <= QChar('9')) || (c >= QChar('a') && c <= QChar('f')))) return false;
    return true;
}
QVariantMap find(const QVariantList &items, const QString &id, const QString &key = QStringLiteral("id")) {
    for (const auto &item : items) {
        const auto row = item.toMap();
        if (row.value(key).toString() == id) return row;
    }
    return {};
}
void remove(QVariantList &items, const QString &id) {
    for (qsizetype i = items.size(); i > 0; --i)
        if (items[i - 1].toMap().value(QStringLiteral("id")).toString() == id) items.removeAt(i - 1);
}
QVariantMap communityRow(const QJsonObject &object) {
    const auto id = object.value(QStringLiteral("id")).toString();
    const auto name = object.value(QStringLiteral("name")).toString();
    const auto role = object.value(QStringLiteral("role")).toString();
    if (!validId(id) || !printableName(name, 100) || !validRole(role)) return {};
    return {{"id", id}, {"name", name}, {"role", role}};
}
QVariantMap channelRow(const QJsonObject &object) {
    const auto id = object.value(QStringLiteral("id")).toString();
    const auto name = object.value(QStringLiteral("name")).toString();
    const auto kind = object.value(QStringLiteral("channel_kind")).toString();
    const int position = object.value(QStringLiteral("position")).toInt(-1);
    if (!validId(id) || !validChannelName(name) || (kind != QStringLiteral("voice") && kind != QStringLiteral("text")) || position < 0) return {};
    return {{"id", id}, {"name", name}, {"channel_kind", kind}, {"position", position}};
}
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
}

ChannelDirectory::ChannelDirectory(AccountSession *account, QObject *parent) : QObject(parent), account_(account) {
    connect(account_, &AccountSession::signedOut, this, &ChannelDirectory::reset);
    connect(account_, &AccountSession::changed, this, [this]() {
        const bool signedIn = account_->signedIn();
        const bool initial = signedIn && !hadSession_;
        hadSession_ = signedIn;
        if (initial) refresh();
    });
    if (account_->signedIn()) { hadSession_ = true; refresh(); }
}
ChannelDirectory::~ChannelDirectory() {
    for (auto *reply : network_.findChildren<QNetworkReply *>()) {
        reply->disconnect(this);
        reply->abort();
    }
}
bool ChannelDirectory::ready() const { return account_->signedIn() && !account_->busy() && !busy(); }
bool ChannelDirectory::canManage() const { return communityRole_ == QStringLiteral("owner") || communityRole_ == QStringLiteral("admin"); }
QString ChannelDirectory::channelName() const { return find(channels_, channelId_).value(QStringLiteral("name")).toString(); }
QString ChannelDirectory::selectedPath() const { return QStringLiteral("/v1/communities/") + communityId_; }
bool ChannelDirectory::validName(const QString &name, int limit) {
    if (printableName(name, limit)) return true;
    status_ = QStringLiteral("Enter a trimmed, printable name within the length limit.");
    emit changed(); return false;
}
bool ChannelDirectory::validChannel(const QString &name, const QString &kind, int position) {
    if (validChannelName(name) && (kind == QStringLiteral("voice") || kind == QStringLiteral("text")) && position >= 0) return true;
    status_ = QStringLiteral("Channel names use 2-80 lowercase letters, digits, - or _. Position must be nonnegative.");
    emit changed(); return false;
}
void ChannelDirectory::clearInvitations() {
    invitationToken_.fill(QChar(0)); invitationToken_.clear();
    invitationId_.clear(); invitations_.clear();
}
void ChannelDirectory::clearSelection() {
    const bool selected = !communityId_.isEmpty() || !channelId_.isEmpty();
    communityId_.clear(); communityName_.clear(); communityRole_.clear(); channelId_.clear();
    channels_.clear(); allChannels_.clear(); members_.clear(); clearInvitations();
    managementLoaded_ = false; detailsWanted_ = false;
    if (selected) emit selectionChanged();
}
void ChannelDirectory::cancel() {
    ++version_;
    if (pending_) { auto *reply = pending_; pending_ = nullptr; reply->abort(); reply->deleteLater(); }
    processing_ = false; detailsWanted_ = false;
    status_ = QStringLiteral("Cancelled. Refresh to confirm whether the change reached the server.");
    emit changed();
}
void ChannelDirectory::reset() {
    cancel();
    clearSelection(); communities_.clear(); outcome_.clear(); hadSession_ = false;
    status_ = QStringLiteral("Sign in to view communities");
    emit changed();
}
void ChannelDirectory::upsertCommunity(const QVariantMap &community) {
    const auto id = community.value(QStringLiteral("id")).toString();
    for (auto &item : communities_) {
        if (item.toMap().value(QStringLiteral("id")).toString() == id) { item = community; return; }
    }
    communities_.prepend(community);
    // Keep a selected community visible even when it falls outside the API's first page.
    while (communities_.size() > 101) communities_.removeLast();
}
void ChannelDirectory::useCommunity(const QVariantMap &community) {
    const auto id = community.value(QStringLiteral("id")).toString();
    const bool switched = id != communityId_;
    if (switched) {
        channels_.clear(); allChannels_.clear(); members_.clear(); channelId_.clear();
        clearInvitations(); managementLoaded_ = false; detailsWanted_ = false;
    }
    communityId_ = id; communityName_ = community.value(QStringLiteral("name")).toString();
    communityRole_ = community.value(QStringLiteral("role")).toString();
    if (!canManage()) clearInvitations();
    upsertCommunity(community);
    if (switched) emit selectionChanged();
}
void ChannelDirectory::refresh() {
    if (!ready()) return;
    outcome_.clear(); detailsWanted_ = managementLoaded_;
    send(Operation::ListCommunities, "GET", QStringLiteral("/v1/communities"));
}
void ChannelDirectory::chooseCommunity(const QString &id) {
    if (!ready()) return;
    const auto community = find(communities_, id);
    if (community.isEmpty()) return;
    outcome_.clear(); useCommunity(community);
    reloadSelected(managementLoaded_);
}
void ChannelDirectory::chooseVoiceChannel(const QString &id) {
    if (!ready() || communityId_.isEmpty() || find(channels_, id).isEmpty() || id == channelId_) return;
    channelId_ = id;
    emit selectionChanged(); emit changed();
}
void ChannelDirectory::loadManagement() {
    if (!ready() || communityId_.isEmpty()) return;
    outcome_.clear(); reloadSelected(true);
}
void ChannelDirectory::reloadSelected(bool management) {
    detailsWanted_ = management;
    send(Operation::GetCommunity, "GET", selectedPath(), {}, communityId_);
}
void ChannelDirectory::createCommunity(const QString &name) {
    if (!ready() || !validName(name, 100)) return;
    outcome_.clear();
    send(Operation::CreateCommunity, "POST", QStringLiteral("/v1/communities"), json({{"name", name}}));
}
void ChannelDirectory::joinCommunity(const QString &token) {
    if (!ready()) return;
    if (!hexToken(token)) { status_ = QStringLiteral("Enter a 64-character community invitation code."); emit changed(); return; }
    outcome_.clear();
    auto payload = json({{"token", token}});
    send(Operation::JoinCommunity, "POST", QStringLiteral("/v1/invitations/accept"), payload);
    payload.fill(0);
}
void ChannelDirectory::renameCommunity(const QString &name) {
    if (!ready() || !canManage() || !validName(name, 100)) return;
    outcome_.clear();
    send(Operation::RenameCommunity, "PATCH", selectedPath(), json({{"name", name}}), communityId_);
}
void ChannelDirectory::deleteCommunity() {
    if (!ready() || communityRole_ != QStringLiteral("owner")) return;
    outcome_.clear();
    send(Operation::DeleteCommunity, "DELETE", selectedPath(), {}, communityId_);
}
void ChannelDirectory::leaveCommunity() {
    if (!ready() || communityId_.isEmpty() || (communityRole_ != QStringLiteral("member") && communityRole_ != QStringLiteral("admin"))) return;
    outcome_.clear();
    send(Operation::LeaveCommunity, "DELETE", selectedPath() + QStringLiteral("/membership"), {}, communityId_);
}
void ChannelDirectory::createChannel(const QString &name, const QString &kind, int position) {
    if (!ready() || !canManage() || !validChannel(name, kind, position)) return;
    outcome_.clear();
    send(Operation::CreateChannel, "POST", selectedPath() + QStringLiteral("/channels"),
         json({{"name", name}, {"channel_kind", kind}, {"position", position}}));
}
void ChannelDirectory::updateChannel(const QString &id, const QString &name, int position) {
    if (!ready() || !canManage()) return;
    const auto channel = find(allChannels_, id);
    if (channel.isEmpty() || !validChannel(name, channel.value(QStringLiteral("channel_kind")).toString(), position)) return;
    outcome_.clear();
    send(Operation::UpdateChannel, "PATCH", selectedPath() + QStringLiteral("/channels/") + id,
         json({{"name", name}, {"position", position}}), id);
}
void ChannelDirectory::deleteChannel(const QString &id) {
    if (!ready() || !canManage() || find(allChannels_, id).isEmpty()) return;
    outcome_.clear();
    send(Operation::DeleteChannel, "DELETE", selectedPath() + QStringLiteral("/channels/") + id, {}, id);
}
void ChannelDirectory::createInvitation() {
    if (!ready() || !canManage()) return;
    outcome_.clear();
    send(Operation::CreateInvitation, "POST", selectedPath() + QStringLiteral("/invitations"));
}
void ChannelDirectory::revokeInvitation(const QString &id) {
    if (!ready() || !canManage() || find(invitations_, id).isEmpty()) return;
    outcome_.clear();
    send(Operation::RevokeInvitation, "DELETE", selectedPath() + QStringLiteral("/invitations/") + id, {}, id);
}
void ChannelDirectory::setMemberRole(const QString &id, const QString &role) {
    if (!ready() || communityRole_ != QStringLiteral("owner")) return;
    const auto member = find(members_, id, QStringLiteral("user_id"));
    if (member.isEmpty() || member.value(QStringLiteral("role")).toString() == QStringLiteral("owner") ||
        (role != QStringLiteral("admin") && role != QStringLiteral("member"))) return;
    outcome_.clear();
    send(Operation::SetMemberRole, "PATCH", selectedPath() + QStringLiteral("/members/") + id, json({{"role", role}}), id);
}
void ChannelDirectory::removeMember(const QString &id) {
    if (!ready() || !canManage()) return;
    const auto member = find(members_, id, QStringLiteral("user_id"));
    const auto role = member.value(QStringLiteral("role")).toString();
    if (member.isEmpty() || role == QStringLiteral("owner") || (communityRole_ == QStringLiteral("admin") && role != QStringLiteral("member"))) return;
    outcome_.clear();
    send(Operation::RemoveMember, "DELETE", selectedPath() + QStringLiteral("/members/") + id, {}, id);
}
void ChannelDirectory::finish() {
    detailsWanted_ = false;
    status_ = !outcome_.isEmpty() ? outcome_ : communityId_.isEmpty() ?
        (communities_.isEmpty() ? QStringLiteral("No communities. Create or join one.") : QStringLiteral("Choose a community")) :
        channels_.isEmpty() ? QStringLiteral("No voice channels. A manager can create one.") : QStringLiteral("Community is up to date");
    outcome_.clear();
}
void ChannelDirectory::send(Operation operation, const QByteArray &method, const QString &path,
                            const QByteArray &payload, const QString &target) {
    if (!account_->signedIn()) return;
    auto url = account_->apiOrigin();
    if (url.scheme() != QStringLiteral("https") &&
        !(url.scheme() == QStringLiteral("http") && (url.host() == QStringLiteral("localhost") || url.host() == QStringLiteral("127.0.0.1")))) return;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + account_->accessToken().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    auto *reply = method == "GET" ? network_.get(request) :
                  method == "POST" ? network_.post(request, payload) : network_.sendCustomRequest(request, method, payload);
    pending_ = reply;
    boundReply(reply, replyLimit, 10000);
    status_ = method == "GET" ? QStringLiteral("Loading community…") : QStringLiteral("Saving changes…");
    const auto version = version_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, operation, target]() {
        if (version != version_ || pending_ != reply) { reply->deleteLater(); return; }
        processing_ = true; pending_ = nullptr;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        auto bytes = reply->readAll();
        reply->deleteLater();
        int expected = 200;
        switch (operation) {
        case Operation::CreateCommunity: case Operation::JoinCommunity:
        case Operation::CreateChannel: case Operation::CreateInvitation: expected = 201; break;
        case Operation::DeleteCommunity: case Operation::LeaveCommunity: case Operation::DeleteChannel:
        case Operation::RevokeInvitation: case Operation::SetMemberRole: case Operation::RemoveMember: expected = 204; break;
        default: break;
        }
        if (error != QNetworkReply::NoError || http != expected || bytes.size() > replyLimit ||
            !applyResponse(operation, bytes, target)) failed(operation, http, error);
        bytes.fill(0);
        processing_ = false;
        emit changed();
    });
    emit changed();
}
bool ChannelDirectory::applyResponse(Operation operation, const QByteArray &bytes, const QString &target) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    switch (operation) {
    case Operation::ListCommunities: case Operation::ListChannels:
    case Operation::ListMembers: case Operation::ListInvitations: {
        const int limit = operation == Operation::ListCommunities ? 100 : 200;
        if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > limit) return false;
        QVariantList parsed;
        QSet<QString> seen;
        for (const auto &value : document.array()) {
            if (!value.isObject()) return false;
            const auto object = value.toObject();
            QVariantMap row;
            QString id;
            if (operation == Operation::ListCommunities) row = communityRow(object);
            else if (operation == Operation::ListChannels) row = channelRow(object);
            else if (operation == Operation::ListMembers) {
                id = object.value(QStringLiteral("user_id")).toString();
                const auto handle = object.value(QStringLiteral("handle")).toString();
                const auto name = object.value(QStringLiteral("display_name")).toString();
                const auto role = object.value(QStringLiteral("role")).toString();
                if (!validId(id) || !printableName(handle, 32) || !printableName(name, 80) || !validRole(role)) return false;
                row = {{"user_id", id}, {"handle", handle}, {"display_name", name}, {"role", role}};
            } else {
                id = object.value(QStringLiteral("id")).toString();
                const int ttl = object.value(QStringLiteral("expires_in")).toInt(-1);
                if (!validId(id) || ttl < 0 || ttl > 86400) return false;
                row = {{"id", id}, {"expires_in", ttl}};
            }
            if (row.isEmpty()) return false;
            if (id.isEmpty()) id = row.value(QStringLiteral("id")).toString();
            if (seen.contains(id)) return false;
            seen.insert(id); parsed.append(row);
        }
        if (operation == Operation::ListCommunities) {
            communities_ = parsed;
            if (!communityId_.isEmpty()) reloadSelected(detailsWanted_); else finish();
        } else if (operation == Operation::ListChannels) {
            allChannels_ = parsed; channels_.clear();
            for (const auto &item : allChannels_)
                if (item.toMap().value(QStringLiteral("channel_kind")).toString() == QStringLiteral("voice")) channels_.append(item);
            if (!channelId_.isEmpty() && find(channels_, channelId_).isEmpty()) { channelId_.clear(); emit selectionChanged(); }
            if (detailsWanted_) send(Operation::ListMembers, "GET", selectedPath() + QStringLiteral("/members")); else finish();
        } else if (operation == Operation::ListMembers) {
            members_ = parsed; managementLoaded_ = true;
            if (canManage()) send(Operation::ListInvitations, "GET", selectedPath() + QStringLiteral("/invitations")); else finish();
        } else {
            invitations_ = parsed;
            if (!invitationId_.isEmpty() && find(invitations_, invitationId_).isEmpty()) {
                invitationToken_.fill(QChar(0)); invitationToken_.clear(); invitationId_.clear();
            }
            finish();
        }
        return true;
    }
    case Operation::GetCommunity: case Operation::CreateCommunity:
    case Operation::JoinCommunity: case Operation::RenameCommunity: {
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        const auto row = communityRow(document.object());
        if (row.isEmpty() || (!target.isEmpty() && row.value(QStringLiteral("id")).toString() != target)) return false;
        useCommunity(row);
        if (operation != Operation::GetCommunity) detailsWanted_ = managementLoaded_;
        if (operation == Operation::CreateCommunity) outcome_ = QStringLiteral("Community created. Add a voice channel and invite your friends.");
        if (operation == Operation::JoinCommunity) outcome_ = QStringLiteral("Joined community.");
        if (operation == Operation::RenameCommunity) outcome_ = QStringLiteral("Community renamed.");
        send(Operation::ListChannels, "GET", selectedPath() + QStringLiteral("/channels"));
        return true;
    }
    case Operation::CreateChannel: case Operation::UpdateChannel: {
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        const auto row = channelRow(document.object());
        if (row.isEmpty() || (!target.isEmpty() && row.value(QStringLiteral("id")).toString() != target)) return false;
        outcome_ = operation == Operation::CreateChannel ? QStringLiteral("Channel created.") : QStringLiteral("Channel updated.");
        reloadSelected(managementLoaded_); return true;
    }
    case Operation::CreateInvitation: {
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        const auto object = document.object();
        const auto token = object.value(QStringLiteral("token")).toString();
        const auto id = object.value(QStringLiteral("id")).toString();
        const int ttl = object.value(QStringLiteral("expires_in")).toInt(-1);
        if (!hexToken(token) || !validId(id) || ttl < 1 || ttl > 86400) return false;
        invitationToken_ = token; invitationId_ = id;
        outcome_ = QStringLiteral("Community code created. Share it privately with a signed-in friend.");
        reloadSelected(true); return true;
    }
    case Operation::DeleteCommunity: case Operation::LeaveCommunity:
        remove(communities_, target); clearSelection();
        outcome_ = operation == Operation::DeleteCommunity ? QStringLiteral("Community deleted.") : QStringLiteral("Left community.");
        send(Operation::ListCommunities, "GET", QStringLiteral("/v1/communities")); return true;
    case Operation::DeleteChannel:
        remove(allChannels_, target); remove(channels_, target);
        if (channelId_ == target) { channelId_.clear(); emit selectionChanged(); }
        outcome_ = QStringLiteral("Channel deleted."); reloadSelected(managementLoaded_); return true;
    case Operation::RevokeInvitation:
        if (target == invitationId_) { invitationToken_.fill(QChar(0)); invitationToken_.clear(); invitationId_.clear(); }
        outcome_ = QStringLiteral("Invitation revoked. Existing memberships are unaffected.");
        reloadSelected(true); return true;
    case Operation::SetMemberRole: case Operation::RemoveMember:
        outcome_ = operation == Operation::SetMemberRole ? QStringLiteral("Member role updated.") : QStringLiteral("Member removed.");
        reloadSelected(true); return true;
    }
    return false;
}
void ChannelDirectory::failed(Operation operation, int http, QNetworkReply::NetworkError error) {
    outcome_.clear(); detailsWanted_ = false;
    if (http == 401) status_ = QStringLiteral("Session changed or expired. Refresh, or sign out and sign in again.");
    else if (http == 403) status_ = QStringLiteral("Your community role does not allow this action.");
    else if (http == 404 && operation == Operation::JoinCommunity) status_ = QStringLiteral("Community code is invalid, expired, revoked or used. Ask a manager for a new code.");
    else if (http == 404) status_ = QStringLiteral("This item is unavailable or your community access changed.");
    else if (http == 409) status_ = operation == Operation::JoinCommunity ? QStringLiteral("You already belong to this community.") : QStringLiteral("That channel name is already in use.");
    else if (http == 400 || http == 422) status_ = QStringLiteral("Check the name, channel type, position or invitation code.");
    else if (http == 429) status_ = QStringLiteral("Too many pending invitations. Revoke an unused code and try again.");
    else if (http == 503) status_ = QStringLiteral("Server is not ready. Ask the owner to check the services.");
    else if (http >= 300 && http < 400) status_ = QStringLiteral("Server redirect blocked. Use the final HTTPS address.");
    else if (error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError) status_ = QStringLiteral("Request timed out or exceeded its response limit. Refresh before retrying a change.");
    else if (error != QNetworkReply::NoError && http == 0) status_ = QStringLiteral("Could not reach the server. Check the address and connection.");
    else status_ = QStringLiteral("Unexpected server response. Refresh before retrying a change.");
    if (http == 401) { clearSelection(); communities_.clear(); }
    else if ((http == 403 || http == 404) && operation != Operation::JoinCommunity && !communityId_.isEmpty()) {
        if (operation == Operation::GetCommunity || operation == Operation::ListChannels || operation == Operation::ListMembers) {
            remove(communities_, communityId_); clearSelection();
        } else {
            // Never retry a mutation. Refresh roles and data after a stale permission/target.
            communityRole_.clear(); members_.clear(); clearInvitations(); managementLoaded_ = false;
            outcome_ = status_;
            reloadSelected(false);
        }
    }
}
