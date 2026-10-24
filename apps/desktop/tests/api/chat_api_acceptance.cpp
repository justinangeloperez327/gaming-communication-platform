#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatSession.h"
#include "FriendsDirectory.h"
#include "ScreenSession.h"
#include "ScreenViewer.h"
#include "BoundedReply.h"
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QFile>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition, int timeout = 8000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2);
    }
    return condition();
}
MessageModel *model(ChatSession &chat) { return static_cast<MessageModel *>(chat.messages()); }
QString channel(const ChannelDirectory &directory, const QString &name) {
    for (const auto &item : directory.allChannels()) {
        const auto row = item.toMap();
        if (row.value("name").toString() == name) return row.value("id").toString();
    }
    return {};
}
QJsonArray publishers(AccountSession& account, ChannelDirectory& rooms) {
    QNetworkAccessManager network; network.setProxy(QNetworkProxy::NoProxy);
    QUrl url(account.apiOrigin()); url.setPath(QStringLiteral("/v1/communities/%1/channels/%2/screen-sessions").arg(rooms.communityId(), rooms.channelId()));
    QNetworkRequest request(url); request.setRawHeader("Authorization", "Bearer " + account.accessToken().toUtf8());
    auto* reply = network.get(request); boundReply(reply, 16384, 5000);
    require(waitFor([&] { return reply->isFinished(); }), "screen directory response deadline");
    require(reply->error() == QNetworkReply::NoError, "real screen directory authorized");
    return QJsonDocument::fromJson(reply->readAll()).array();
}
QString find(ChatSession &chat, const QString &body) {
    for (const auto &row : model(chat)->rows()) if (row.body == body) return row.id;
    return {};
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto origin = qEnvironmentVariable("GCP_CHAT_ORIGIN");
        const auto password = qEnvironmentVariable("GCP_CHAT_PASSWORD");
        require(!origin.isEmpty() && !password.isEmpty(), "disposable acceptance environment required");
        AccountSession owner, friendAccount;
        ChannelDirectory ownerRooms(&owner), friendRooms(&friendAccount);
        FriendsDirectory ownerFriends(&owner), peerFriends(&friendAccount);
        ChatSession ownerChat(&owner, &ownerRooms);
        ChatSession ownerDirect(&owner, &ownerFriends), peerDirect(&friendAccount, &peerFriends);
        owner.signIn(origin, "chat-owner@example.test", password);
        require(waitFor([&]() { return owner.signedIn() && !owner.busy() && !ownerRooms.busy() &&
            !owner.invitationBusy() && !owner.invitationStatus().isEmpty(); }) && owner.serverOwner(), "real owner login and initial invitation list");
        owner.inviteFriend("chat-friend@example.test");
        require(waitFor([&]() { return !owner.invitationBusy(); }) && !owner.invitationCode().isEmpty(), "owner account invitation");
        friendAccount.createAccount(origin, "chat_friend", "Chat Friend", "chat-friend@example.test", password, owner.invitationCode());
        require(waitFor([&]() { return friendAccount.signedIn() && !friendRooms.busy(); }), "invited friend registration");
        ownerRooms.createCommunity("Desktop chat acceptance");
        require(waitFor([&]() { return !ownerRooms.busy(); }) && !ownerRooms.communityId().isEmpty(), "community creation");
        ownerRooms.createChannel("general", "text", 0);
        require(waitFor([&]() { return !ownerRooms.busy(); }) && !channel(ownerRooms, "general").isEmpty(), "text channel creation");
        ownerRooms.createChannel("games", "text", 1);
        require(waitFor([&]() { return !ownerRooms.busy(); }) && !channel(ownerRooms, "games").isEmpty(), "second text channel creation");
        ownerRooms.createChannel("team-voice", "voice", 2);
        require(waitFor([&]() { return !ownerRooms.busy(); }) && !channel(ownerRooms, "team-voice").isEmpty(), "voice channel creation");
        ownerRooms.createInvitation();
        require(waitFor([&]() { return !ownerRooms.busy(); }) && !ownerRooms.invitationToken().isEmpty(), "community invitation");
        friendRooms.joinCommunity(ownerRooms.invitationToken());
        require(waitFor([&]() { return !friendRooms.busy(); }) && friendRooms.communityId() == ownerRooms.communityId(), "friend community membership");
        const auto general = channel(ownerRooms, "general"), games = channel(ownerRooms, "games"), voice = channel(ownerRooms, "team-voice");
        ownerRooms.chooseVoiceChannel(voice);
        const auto fixture = qEnvironmentVariable("GCP_SCREEN_FIXTURE_PROGRAM");
        require(!fixture.isEmpty(), "test-only screen protocol fixture required");
        QTemporaryFile screenTrace; require(screenTrace.open(), "screen trace file");
        qputenv("GCP_SCREEN_FIXTURE_LOG", screenTrace.fileName().toUtf8());
        ScreenSession ownerScreen(&owner, &ownerRooms, nullptr, {fixture, 200});
        require(publishers(owner, ownerRooms).isEmpty(), "no publisher before consent");
        ownerScreen.chooseSource();
        require(waitFor([&] { return ownerScreen.awaitingConsent(); }), "test source retained by child");
        require(publishers(owner, ownerRooms).isEmpty(), "source selection alone creates no real lease");
        ownerScreen.confirmShare(true);
        require(waitFor([&] { return ownerScreen.sharing(); }), "real Qt grant validates Rust signature payload and starts fixture");
        auto current = publishers(owner, ownerRooms);
        require(current.size() == 1 && current[0].toObject().value("user_id").toString() == owner.userId(), "real directory identifies current publishing account");
        require(waitFor([&] {
            QFile file(screenTrace.fileName()); if (!file.open(QIODevice::ReadOnly)) return false;
            return file.readAll().count("renew\n") >= 2;
        }), "actual Rust screen lease renewed twice through native control protocol");
        const auto viewFixture = qEnvironmentVariable("GCP_VIEW_FIXTURE_PROGRAM");
        require(!viewFixture.isEmpty(), "test-only viewer protocol fixture required");
        ScreenViewer actualViewer(&owner, &ownerRooms, nullptr, {viewFixture, 200});
        actualViewer.refreshPublishers();
        require(waitFor([&] { return !actualViewer.busy() && actualViewer.publishers().size() == 1; }), "real viewer publisher discovery");
        actualViewer.choosePublisher(current[0].toObject().value("id").toString());
        actualViewer.watch(false); require(!actualViewer.busy(), "viewer consent false creates no lease");
        actualViewer.watch(true);
        require(waitFor([&] { return !actualViewer.busy(); }) && !actualViewer.watching() && actualViewer.image().isNull(), "real Rust-issued viewer lease cannot fabricate pixels without an actual SFU publisher");
        require(actualViewer.status().contains("service") && publishers(owner,ownerRooms).size() == 1 && ownerScreen.sharing(), "missing SFU track fails closed and leaves the independent publisher intact");
        ownerScreen.stop(); require(waitFor([&] { return !ownerScreen.busy(); }), "real lease Stop and child exit");
        require(publishers(owner, ownerRooms).isEmpty() && ownerRooms.channelId() == voice, "real stopped lease removed without changing voice selection");
        friendRooms.chooseVoiceChannel(voice);
        ScreenSession friendScreen(&friendAccount, &friendRooms, nullptr, {fixture, 200});
        int voiceSelections = 0;
        QObject::connect(&ownerRooms, &ChannelDirectory::selectionChanged, &app, [&]() { ++voiceSelections; });
        ownerRooms.chooseTextChannel(general); friendRooms.chooseTextChannel(general);
        auto receiver = std::make_unique<ChatSession>(&friendAccount, &friendRooms);
        require(waitFor([&]() { return ownerChat.live() && receiver->live() && ownerChat.canMutate() && receiver->canMutate(); }), "both desktop controllers authorized and subscribed to real Rust sockets");
        require(model(ownerChat)->rowCount() == 0 && model(*receiver)->rowCount() == 0, "new channel starts empty");
        ownerChat.setDraft("hello <b>literal</b> \xF0\x9F\x8E\xAE"); ownerChat.sendDraft();
        require(waitFor([&]() { return !ownerChat.sending() && ownerChat.draft().isEmpty() && model(*receiver)->rowCount() == 1; }), "real HTTP send and socket-triggered friend delivery");
        const auto first = model(ownerChat)->rows().last().id;
        require(model(*receiver)->find(first)->body == model(ownerChat)->find(first)->body, "peer receives exact stored plaintext");
        ownerChat.editMessage(first, "edited by author");
        require(waitFor([&]() { return !ownerChat.sending() && model(*receiver)->find(first)->body == "edited by author"; }), "real author edit reaches peer");
        receiver->editMessage(first, "forbidden");
        require(!receiver->sending() && model(*receiver)->find(first)->body == "edited by author", "member cannot edit another author's message");
        receiver->setDraft("friend reply"); receiver->sendDraft();
        require(waitFor([&]() { return !receiver->sending() && receiver->draft().isEmpty() && !find(ownerChat, "friend reply").isEmpty(); }), "friend to owner delivery");
        const auto reply = find(ownerChat, "friend reply"); ownerChat.deleteMessage(reply);
        require(waitFor([&]() { return !ownerChat.sending() && model(*receiver)->find(reply) && model(*receiver)->find(reply)->deletedMs; }), "owner deletion reaches friend as tombstone");
        // Tear down an actual subscriber while messages change, then construct a
        // fresh one. Forced interruption/resync of a retained controller is in
        // chat_tests; here the real API must provide the full current snapshot.
        receiver.reset();
        ownerChat.editMessage(first, "edited while peer offline");
        require(waitFor([&]() { return !ownerChat.sending() && model(ownerChat)->find(first)->body == "edited while peer offline"; }), "offline edit saved");
        ownerChat.setDraft("missed while offline"); ownerChat.sendDraft();
        require(waitFor([&]() { return !ownerChat.sending() && ownerChat.draft().isEmpty(); }), "offline creation saved");
        receiver = std::make_unique<ChatSession>(&friendAccount, &friendRooms);
        require(waitFor([&]() { return receiver->live() && receiver->canMutate() && model(*receiver)->find(first) &&
            model(*receiver)->find(first)->body == "edited while peer offline" && !find(*receiver, "missed while offline").isEmpty() && model(*receiver)->find(reply) && model(*receiver)->find(reply)->deletedMs; }), "real resubscribe snapshot restores missed content and tombstones");
        ownerRooms.chooseTextChannel(games);
        require(waitFor([&]() { return ownerChat.canMutate() && !ownerChat.loading() && model(ownerChat)->rowCount() == 0; }), "second text history isolated");
        require(voiceSelections == 0 && ownerRooms.channelId() == voice, "text switch leaves voice selection untouched");
        ownerRooms.chooseTextChannel(general);
        require(waitFor([&]() { return ownerChat.canMutate() && !ownerChat.loading(); }), "restore general");
        receiver->setDraft("do not send after revocation");
        // The existing directory intentionally has no live member updates.
        // Reload after the friend's join before invoking its guarded removal.
        ownerRooms.loadManagement();
        require(waitFor([&]() { return !ownerRooms.busy() && ownerRooms.members().size() == 2; }), "refresh current members");
        friendScreen.chooseSource();
        require(waitFor([&] { return friendScreen.awaitingConsent(); }), "friend source selection");
        friendScreen.confirmShare(true);
        require(waitFor([&] { return friendScreen.sharing(); }), "friend screen lease before membership removal");
        ownerRooms.removeMember(friendAccount.userId());
        require(waitFor([&]() { return !ownerRooms.busy() && ownerRooms.members().size() == 1; }), "member removal");
        require(waitFor([&] { return !friendScreen.busy(); }) && !friendScreen.sharing() && friendScreen.status().contains("access"), "real membership removal denies renewal and terminates native peer");
        require(publishers(owner, ownerRooms).isEmpty(), "revoked publisher disappears from real directory");
        receiver->refreshLatest();
        require(waitFor([&]() { return !receiver->loading() && !receiver->canMutate() && model(*receiver)->rowCount() == 0 && receiver->draft().isEmpty(); }), "real authorization loss purges peer history and draft");
        receiver->sendDraft(); require(!receiver->sending(), "no automatic delivery after access removal");
        // Friends and DMs are independent of shared-community membership.
        require(waitFor([&]() { return ownerFriends.verified() && peerFriends.verified() && !ownerFriends.busy() && !peerFriends.busy(); }), "initial social snapshots");
        const auto requestFriend = [&]() {
            ownerFriends.sendRequest("chat_friend");
            require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.requests().size() == 1; }), "outgoing friend request");
            peerFriends.refresh();
            require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.requests().size() == 1; }), "incoming friend request");
            return peerFriends.requests().first().toMap().value("id").toString();
        };
        auto requestId = requestFriend();
        require(peerFriends.requests().first().toMap().value("user_id").toString() == owner.userId(), "pending request profile belongs to the other account");
        ownerFriends.acceptRequest(requestId); require(!ownerFriends.busy(), "sender cannot accept own outgoing request");
        ownerFriends.cancelRequest(requestId);
        require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.requests().isEmpty(); }), "sender cancels pending request");
        peerFriends.refresh(); require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.requests().isEmpty(); }), "cancel removed recipient request");
        requestId = requestFriend(); peerFriends.declineRequest(requestId);
        require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.requests().isEmpty(); }), "recipient decline");
        ownerFriends.refresh(); require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.requests().isEmpty(); }), "decline removed sender pending state");
        requestId = requestFriend(); peerFriends.acceptRequest(requestId);
        require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.friends().size() == 1; }), "recipient accepts friendship");
        ownerFriends.refresh(); require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.friends().size() == 1; }), "sender discovers friendship");
        ownerFriends.openConversation(friendAccount.userId());
        require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && !ownerFriends.conversationId().isEmpty(); }), "open real private conversation");
        const auto privateId = ownerFriends.conversationId();
        peerFriends.refresh(); require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.conversations().size() == 1; }), "recipient discovers private conversation");
        peerFriends.chooseConversation(privateId);
        require(waitFor([&]() { return ownerDirect.live() && peerDirect.live() && ownerDirect.canMutate() && peerDirect.canMutate(); }), "actual DM sockets and history authorized");
        require(!ownerDirect.canModerate() && !peerDirect.canModerate(), "community ownership never grants DM moderation rights");
        ownerDirect.setDraft("private 🎮 <b>literal</b>"); ownerDirect.sendDraft();
        require(waitFor([&]() { return !ownerDirect.sending() && ownerDirect.draft().isEmpty() && model(peerDirect)->rowCount() == 1; }), "private send and live delivery");
        const auto privateMessage = model(ownerDirect)->rows().last().id;
        require(model(peerDirect)->find(privateMessage)->body == "private 🎮 <b>literal</b>", "private Unicode text is exact");
        peerDirect.editMessage(privateMessage, "forbidden"); peerDirect.deleteMessage(privateMessage);
        require(!peerDirect.sending(), "peer cannot rewrite or delete another DM author's message");
        ownerDirect.editMessage(privateMessage, "edited private text");
        require(waitFor([&]() { return !ownerDirect.sending() && model(peerDirect)->find(privateMessage)->body == "edited private text"; }), "private live edit");
        peerDirect.setDraft("private reply"); peerDirect.sendDraft();
        require(waitFor([&]() { return !peerDirect.sending() && peerDirect.draft().isEmpty() && !find(ownerDirect, "private reply").isEmpty(); }), "private bidirectional delivery");
        const auto privateReply = find(ownerDirect, "private reply"); ownerDirect.deleteMessage(privateReply);
        require(!ownerDirect.sending(), "community owner cannot delete a peer's DM");
        ownerDirect.deleteMessage(privateMessage);
        require(waitFor([&]() { return !ownerDirect.sending() && model(peerDirect)->find(privateMessage)->deletedMs; }), "private tombstone delivery");
        peerFriends.closeConversation(); require(model(peerDirect)->rowCount() == 0, "closing private chat clears visible history");
        ownerDirect.setDraft("private offline creation"); ownerDirect.sendDraft();
        require(waitFor([&]() { return !ownerDirect.sending() && ownerDirect.draft().isEmpty(); }), "private offline send");
        const auto offlinePrivate = find(ownerDirect, "private offline creation"); ownerDirect.editMessage(offlinePrivate, "private offline edit");
        require(waitFor([&]() { return !ownerDirect.sending() && model(ownerDirect)->find(offlinePrivate)->body == "private offline edit"; }), "private offline edit saved");
        peerFriends.chooseConversation(privateId);
        require(waitFor([&]() { return peerDirect.live() && peerDirect.canMutate() && model(peerDirect)->find(offlinePrivate) && model(peerDirect)->find(offlinePrivate)->body == "private offline edit" && model(peerDirect)->find(privateMessage)->deletedMs; }), "private resubscribe recovers edits and deletions");
        ownerDirect.setDraft("owner draft before block"); peerDirect.setDraft("peer draft before block");
        ownerFriends.blockUser(friendAccount.userId());
        require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.blocks().size() == 1 && ownerFriends.conversationId().isEmpty() && peerFriends.conversationId().isEmpty() && model(ownerDirect)->rowCount() == 0 && model(peerDirect)->rowCount() == 0; }), "blocking revokes both active desktop DMs");
        require(ownerDirect.draft().isEmpty() && peerDirect.draft().isEmpty() && !ownerDirect.canSubmit() && !peerDirect.canSubmit(), "blocking clears both drafts without delivery");
        ownerFriends.unblockUser(friendAccount.userId());
        require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.blocks().isEmpty() && ownerFriends.friends().isEmpty(); }), "unblocking does not restore friendship");
        peerFriends.refresh(); require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy(); }), "peer social recovery");
        requestId = requestFriend(); peerFriends.acceptRequest(requestId);
        require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.friends().size() == 1; }), "explicit re-friend");
        ownerFriends.refresh(); require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.friends().size() == 1; }), "sender re-friend snapshot");
        ownerFriends.openConversation(friendAccount.userId());
        require(waitFor([&]() { return ownerFriends.verified() && !ownerFriends.busy() && ownerFriends.conversationId() == privateId && ownerDirect.canMutate(); }), "re-friend reopens the retained canonical conversation");
        peerFriends.refresh(); require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy(); }), "peer conversation list refreshed");
        peerFriends.chooseConversation(privateId);
        require(waitFor([&]() { return peerDirect.canMutate() && peerDirect.live(); }), "peer reopens retained DM");
        peerFriends.removeFriend(owner.userId());
        require(waitFor([&]() { return peerFriends.verified() && !peerFriends.busy() && peerFriends.friends().isEmpty() && peerFriends.conversationId().isEmpty() && ownerFriends.conversationId().isEmpty(); }), "unfriend revokes both DM sessions");
        require(voiceSelections == 0 && ownerRooms.channelId() == voice, "all social/private operations preserve the independent voice selection");
        ownerRooms.deleteCommunity();
        require(waitFor([&]() { return !ownerRooms.busy() && ownerRooms.communityId().isEmpty(); }), "test community cleanup");
        friendAccount.signOut(); owner.signOut();
        std::cout << "Real Qt controllers -> Rust HTTP/WebSockets -> PostgreSQL channel/friends/DM acceptance passed. Real screen lease create/renew/stop/revocation and viewer discovery/negative live-track authorization also passed with test-only native protocol peers. No physical capture, media or deployed service tested.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
