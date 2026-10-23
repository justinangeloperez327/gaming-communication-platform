#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatSession.h"
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
        ChatSession ownerChat(&owner, &ownerRooms);
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
        ownerRooms.removeMember(friendAccount.userId());
        require(waitFor([&]() { return !ownerRooms.busy() && ownerRooms.members().size() == 1; }), "member removal");
        receiver->refreshLatest();
        require(waitFor([&]() { return !receiver->loading() && !receiver->canMutate() && model(*receiver)->rowCount() == 0 && receiver->draft().isEmpty(); }), "real authorization loss purges peer history and draft");
        receiver->sendDraft(); require(!receiver->sending(), "no automatic delivery after access removal");
        ownerRooms.deleteCommunity();
        require(waitFor([&]() { return !ownerRooms.busy() && ownerRooms.communityId().isEmpty(); }), "test community cleanup");
        friendAccount.signOut(); owner.signOut();
        std::cout << "Real Qt controllers -> Rust HTTP/WebSockets -> PostgreSQL acceptance passed. No physical media or deployed service tested.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
