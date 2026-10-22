#include "AccountSession.h"
#include "ChannelDirectory.h"
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition) {
    if (condition()) return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (condition()) loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    poll.start(); loop.exec();
    return condition();
}
QString uuid(int n) { return QStringLiteral("00000000-0000-0000-0000-%1").arg(n, 12, 16, QChar('0')); }
struct Space {
    QString id, name, role;
    QJsonArray channels, members, invitations;
};
// A protocol fixture, not a replacement for the PostgreSQL authorization tests.
struct Api {
    QTcpServer server;
    QMap<QString, Space> spaces;
    QString overrideRole;
    bool oversizedChannels = false, malformedChannels = false, holdChannels = false, denyCommunity = false;
    int mutations = 0, held = 0, disconnected = 0, nextSpace = 1, nextChannel = 100, nextInvite = 400;
    QJsonObject lastMutation;
    QString lastMutationPath;
    static void response(QTcpSocket *socket, int status, const QByteArray &bytes = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
    static void response(QTcpSocket *socket, int status, const QJsonObject &object) { response(socket, status, QJsonDocument(object).toJson(QJsonDocument::Compact)); }
    static void response(QTcpSocket *socket, int status, const QJsonArray &array) { response(socket, status, QJsonDocument(array).toJson(QJsonDocument::Compact)); }
    static void error(QTcpSocket *socket, int status) { response(socket, status, QByteArray("{\"error\":\"private-server-detail\"}")); }
    QString role(const Space &space) const { return overrideRole.isEmpty() ? space.role : overrideRole; }
    QJsonObject view(const Space &space) const { return {{"id", space.id}, {"name", space.name}, {"role", role(space)}}; }
    QString addSpace(const QString &name, const QString &role) {
        const auto id = uuid(nextSpace++);
        const auto owner = role == "owner" ? uuid(700) : uuid(703);
        Space space{id, name, role, {}, {}, {}};
        space.members = QJsonArray{
            QJsonObject{{"user_id", owner}, {"handle", "owner"}, {"display_name", "Owner"}, {"role", "owner"}},
            QJsonObject{{"user_id", uuid(701)}, {"handle", "admin"}, {"display_name", "Admin"}, {"role", "admin"}},
            QJsonObject{{"user_id", role == "owner" ? uuid(702) : uuid(700)}, {"handle", "friend"}, {"display_name", "Friend"}, {"role", "member"}}
        };
        spaces.insert(id, space);
        return id;
    }
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QString &path, const QJsonObject &body, bool authorized) {
        if (path == "/v1/auth/login") {
            response(socket, 200, QJsonObject{{"access_token", QString(64, 'a')}, {"refresh_token", QString(64, 'b')},
                {"expires_in", 900}, {"server_owner", false}, {"user", QJsonObject{{"id", uuid(700)}, {"display_name", "Owner"}}}});
            return;
        }
        if (!authorized) { error(socket, 401); return; }
        if (path == "/v1/auth/logout") { response(socket, 204); return; }
        if (method != "GET") { ++mutations; lastMutation = body; lastMutationPath = path; }
        if (path == "/v1/communities" && method == "GET") {
            QJsonArray list; for (const auto &space : spaces) list.append(view(space));
            response(socket, 200, list); return;
        }
        if (path == "/v1/communities" && method == "POST") {
            const auto id = addSpace(body.value("name").toString(), "owner");
            response(socket, 201, view(spaces[id])); return;
        }
        if (path == "/v1/invitations/accept") {
            if (body.value("token").toString() != QString(64, 'd')) { error(socket, 404); return; }
            const auto id = addSpace("Joined community", "member");
            response(socket, 201, view(spaces[id])); return;
        }
        const auto parts = path.split('/');
        if (parts.size() < 4 || !spaces.contains(parts[3])) { error(socket, 404); return; }
        auto &space = spaces[parts[3]];
        const auto currentRole = role(space);
        const bool manager = currentRole == "owner" || currentRole == "admin";
        if (parts.size() == 4) {
            if (method == "GET") { if (denyCommunity) error(socket, 404); else response(socket, 200, view(space)); return; }
            if (method == "PATCH" && manager) { space.name = body.value("name").toString(); response(socket, 200, view(space)); return; }
            if (method == "DELETE" && currentRole == "owner") { spaces.remove(parts[3]); response(socket, 204); return; }
            error(socket, 403); return;
        }
        if (parts[4] == "membership" && method == "DELETE") {
            if (currentRole == "owner") { error(socket, 403); return; }
            spaces.remove(parts[3]); response(socket, 204); return;
        }
        if (parts[4] == "channels") {
            if (method == "GET") {
                if (holdChannels) {
                    ++held; QObject::connect(socket, &QTcpSocket::disconnected, &server, [this]() { ++disconnected; }); return;
                }
                if (oversizedChannels) { response(socket, 200, QByteArray(160000, 'x')); return; }
                if (malformedChannels) { response(socket, 200, QJsonArray{QJsonObject{{"id", "invalid"}}}); return; }
                auto sorted = space.channels.toVariantList();
                std::sort(sorted.begin(), sorted.end(), [](const QVariant &a, const QVariant &b) {
                    return a.toMap().value("position").toInt() < b.toMap().value("position").toInt();
                });
                response(socket, 200, QJsonArray::fromVariantList(sorted)); return;
            }
            if (!manager) { error(socket, 403); return; }
            if (method == "POST") {
                auto channel = body; channel.insert("id", uuid(nextChannel++)); space.channels.append(channel);
                response(socket, 201, channel); return;
            }
            for (qsizetype i = 0; i < space.channels.size(); ++i) {
                auto channel = space.channels[i].toObject();
                if (parts.size() != 6 || channel.value("id").toString() != parts[5]) continue;
                if (method == "PATCH") {
                    channel.insert("name", body.value("name")); channel.insert("position", body.value("position"));
                    space.channels[i] = channel; response(socket, 200, channel); return;
                }
                if (method == "DELETE") { space.channels.removeAt(i); response(socket, 204); return; }
            }
        }
        if (parts[4] == "members") {
            if (method == "GET") { response(socket, 200, space.members); return; }
            for (qsizetype i = 0; i < space.members.size(); ++i) {
                auto member = space.members[i].toObject();
                if (parts.size() != 6 || member.value("user_id").toString() != parts[5]) continue;
                const auto targetRole = member.value("role").toString();
                if (method == "PATCH" && currentRole == "owner" && targetRole != "owner") {
                    member.insert("role", body.value("role")); space.members[i] = member; response(socket, 204); return;
                }
                if (method == "DELETE" && targetRole != "owner" && (currentRole == "owner" || (currentRole == "admin" && targetRole == "member"))) {
                    space.members.removeAt(i); response(socket, 204); return;
                }
                error(socket, 403); return;
            }
        }
        if (parts[4] == "invitations") {
            if (!manager) { error(socket, 403); return; }
            if (method == "GET") { response(socket, 200, space.invitations); return; }
            if (method == "POST") {
                QJsonObject invitation{{"id", uuid(nextInvite++)}, {"expires_in", 86400}};
                space.invitations.append(invitation); invitation.insert("token", QString(64, 'c'));
                response(socket, 201, invitation); return;
            }
            if (method == "DELETE" && parts.size() == 6) {
                for (qsizetype i = 0; i < space.invitations.size(); ++i) {
                    if (space.invitations[i].toObject().value("id").toString() == parts[5]) {
                        space.invitations.removeAt(i); response(socket, 204); return;
                    }
                }
            }
        }
        error(socket, 404);
    }
    Api() {
        require(server.listen(QHostAddress::LocalHost), "fixture listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("answered").toBool()) return;
                    int length = 0; bool authorized = false;
                    for (const auto &line : bytes.left(end).split('\n')) {
                        const auto colon = line.indexOf(':'); if (colon < 0) continue;
                        const auto name = line.left(colon).trimmed().toLower(); const auto value = line.mid(colon + 1).trimmed();
                        if (name == "content-length") length = value.toInt();
                        if (name == "authorization") authorized = value == "Bearer " + QByteArray(64, 'a');
                    }
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto request = bytes.left(bytes.indexOf("\r\n")).split(' ');
                    dispatch(socket, request.value(0), QString::fromUtf8(request.value(1)),
                             QJsonDocument::fromJson(bytes.mid(end + 4, length)).object(), authorized);
                });
            }
        });
    }
    ~Api() {
        server.close();
        for (auto *socket : server.findChildren<QTcpSocket *>()) { socket->disconnect(); socket->abort(); }
    }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        Api api;
        AccountSession account;
        ChannelDirectory directory(&account);
        int selections = 0;
        QObject::connect(&directory, &ChannelDirectory::selectionChanged, &directory, [&]() { ++selections; });
        account.signIn(api.origin(), "owner@example.test", "private-password-123");
        require(waitFor([&]() { return account.signedIn() && !directory.busy(); }), "sign in and load empty directory");
        require(directory.communities().isEmpty() && !account.serverOwner(), "community creation is independent of server ownership");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("directoryController", &directory);
        engine.rootContext()->setContextProperty("accountController", &account);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nCommunityPanel { directory: directoryController; signedIn: accountController.signedIn; width: 600 }",
                          QUrl::fromLocalFile(QStringLiteral(GCP_COMMUNITY_QML_DIR) + "/CommunityPanelHarness.qml"));
        QQuickWindow window;
        window.resize(650, 800);
        const std::unique_ptr<QObject> panel(component.create());
        if (!panel) std::cerr << component.errorString().toStdString();
        require(panel != nullptr, "actual community panel QML loads");
        qobject_cast<QQuickItem *>(panel.get())->setParentItem(window.contentItem());
        window.show();
        const auto field = [&](const char *name) {
            auto *object = panel->findChild<QObject *>(QString::fromLatin1(name));
            require(object != nullptr, "QML control exists"); return object;
        };
        const auto click = [&](const char *name) { require(QMetaObject::invokeMethod(field(name), "clicked"), "QML button action"); };
        const auto idle = [&]() { return !directory.busy(); };
        const auto settled = [&]() { require(waitFor(idle), "directory workflow finished"); };
        const auto confirm = [&](const QString &action, const QString &id, const QString &name, const QString &role = {}) {
            panel->setProperty("pendingAction", action); panel->setProperty("pendingCommunity", directory.communityId());
            panel->setProperty("pendingId", id); panel->setProperty("pendingName", name); panel->setProperty("pendingRole", role);
            require(QMetaObject::invokeMethod(panel.get(), "confirmAction"), "QML confirmation action");
        };
        field("newCommunityName")->setProperty("text", "My friends"); click("createCommunity"); settled();
        const auto community = directory.communityId();
        require(!community.isEmpty() && directory.communityRole() == "owner" && directory.communityName() == "My friends", "QML creates and selects community");
        require(directory.channelId().isEmpty(), "new community does not select a room");
        field("newChannelName")->setProperty("text", "team-voice"); click("createChannel"); settled();
        require(directory.channels().size() == 1 && api.lastMutation.value("channel_kind").toString() == "voice", "QML creates voice channel with actual request JSON");
        const auto voice = directory.channels().first().toMap().value("id").toString();
        directory.chooseVoiceChannel(voice);
        const int selected = selections;
        field("newChannelName")->setProperty("text", "general"); field("newChannelKind")->setProperty("currentIndex", 1); click("createChannel"); settled();
        require(directory.allChannels().size() == 2 && directory.channels().size() == 1 && selections == selected, "text channels remain separate from voice selection");
        field("communityManagement")->setProperty("checked", true); settled();
        require(directory.managementLoaded() && directory.members().size() == 3, "lazy member and invitation management loads");
        require(QMetaObject::invokeMethod(panel.get(), "selectChannel", Q_ARG(QVariant, QVariant(voice))), "select channel editor by ID");
        field("editChannelName")->setProperty("text", "renamed-voice"); field("editChannelPosition")->setProperty("value", 17); click("saveChannel"); settled();
        require(directory.channelId() == voice && directory.channelName() == "renamed-voice" && selections == selected, "rename and reorder preserve selected room");
        require(field("manageChannelPicker")->property("currentValue").toString() == voice && api.lastMutation.value("position").toInt() == 17, "channel editor follows stable ID after reorder");
        field("renameCommunityName")->setProperty("text", "My squad"); click("renameCommunity"); settled();
        require(directory.communityName() == "My squad" && selections == selected, "community rename preserves selection");
        click("generateCommunityCode"); settled();
        require(directory.invitationToken().size() == 64 && directory.invitations().size() == 1, "community code issuance and pending list");
        directory.revokeInvitation(directory.invitationId()); settled();
        require(directory.invitationToken().isEmpty() && directory.invitations().isEmpty(), "revocation clears displayed code");
        confirm("role", uuid(702), "friend", "admin"); settled();
        require(api.lastMutation.value("role").toString() == "admin", "QML role confirmation sends admin change");
        const int beforeOwnerGuard = api.mutations;
        directory.removeMember(uuid(700)); directory.setMemberRole(uuid(700), "member");
        require(api.mutations == beforeOwnerGuard && !directory.busy(), "owner membership cannot be removed or demoted locally");
        confirm("removeMember", uuid(702), "friend"); settled();
        require(directory.members().size() == 2, "owner removes an admin through confirmation");
        field("confirmCommunityName")->setProperty("text", "wrong");
        confirm("deleteCommunity", community, directory.communityName());
        require(!directory.busy() && api.mutations == beforeOwnerGuard + 1, "wrong typed confirmation cannot delete community");
        panel->setProperty("pendingAction", "deleteChannel"); panel->setProperty("pendingCommunity", uuid(999)); panel->setProperty("pendingId", voice);
        QMetaObject::invokeMethod(panel.get(), "confirmAction");
        require(!directory.busy() && api.mutations == beforeOwnerGuard + 1, "stale confirmation cannot affect another community");
        confirm("deleteChannel", voice, "renamed-voice"); settled();
        require(directory.channelId().isEmpty() && directory.channels().isEmpty() && selections == selected + 1, "deleting selected voice channel invalidates room once");
        const auto previous = directory.allChannels();
        api.oversizedChannels = true; directory.refresh(); settled();
        require(directory.allChannels() == previous && directory.status().contains("limit"), "oversized replies cannot corrupt cached channels");
        api.oversizedChannels = false; api.malformedChannels = true; directory.refresh(); settled();
        require(directory.allChannels() == previous && directory.status().contains("Unexpected"), "invalid wire rows reject the whole response");
        api.malformedChannels = false;
        api.holdChannels = true; const int beforeHeld = api.held; const int beforeClosed = api.disconnected;
        directory.refresh(); require(waitFor([&]() { return api.held > beforeHeld; }), "held refresh transport");
        directory.cancel(); require(waitFor([&]() { return api.disconnected > beforeClosed; }) && !directory.busy(), "cancel aborts actual HTTP transport");
        api.holdChannels = false; directory.refresh(); settled();
        click("generateCommunityCode"); settled();
        field("communityJoinCode")->setProperty("text", QString(64, 'd'));
        api.holdChannels = true; const int beforeLogout = api.held;
        directory.refresh(); require(waitFor([&]() { return api.held > beforeLogout; }), "held request before logout");
        account.signOut();
        require(directory.communities().isEmpty() && directory.members().isEmpty() && directory.invitationToken().isEmpty() && directory.communityId().isEmpty(), "logout clears directory and privileged data");
        require(field("communityJoinCode")->property("text").toString().isEmpty(), "QML clears undispatched join code at logout");
        api.holdChannels = false;
        account.signIn(api.origin(), "owner@example.test", "private-password-123");
        require(waitFor([&]() { return account.signedIn() && !directory.busy() && !directory.communities().isEmpty(); }), "relogin reloads directory");
        directory.chooseCommunity(community); settled();
        field("confirmCommunityName")->setProperty("text", directory.communityName());
        confirm("deleteCommunity", community, directory.communityName()); settled();
        require(directory.communityId().isEmpty() && directory.communities().isEmpty(), "confirmed delete clears selection and directory");
        field("communityJoinCode")->setProperty("text", QString(64, 'f')); click("joinCommunity"); settled();
        require(directory.status().contains("invalid") && !directory.status().contains("private-server-detail"), "join errors are actionable and static");
        field("communityJoinCode")->setProperty("text", QString(64, 'd')); click("joinCommunity"); settled();
        require(directory.communityRole() == "member" && !directory.canManage(), "QML joins as ordinary member");
        field("communityManagement")->setProperty("checked", true); settled();
        require(!field("communityManagerControls")->property("visible").toBool(), "member management controls hidden");
        const int memberMutations = api.mutations;
        directory.createChannel("forbidden", "voice", 0); directory.deleteCommunity(); directory.createInvitation(); directory.setMemberRole(uuid(701), "member");
        require(!directory.busy() && api.mutations == memberMutations, "member cannot invoke privileged controller actions");
        api.overrideRole = "admin"; directory.loadManagement(); settled();
        require(directory.communityRole() == "admin" && directory.canManage(), "fresh role enables admin actions");
        directory.setMemberRole(uuid(701), "member"); directory.removeMember(uuid(701));
        require(!directory.busy() && api.mutations == memberMutations, "admin cannot demote or remove another admin");
        api.overrideRole = "member"; directory.renameCommunity("stale admin"); settled();
        require(directory.communityRole() == "member" && !directory.canManage() && directory.status().contains("role"), "403 revalidates stale role without retrying mutation");
        require(api.mutations == memberMutations + 1 && directory.invitations().isEmpty(), "permission failure performs only one mutation and clears privileged state");
        confirm("leave", "", directory.communityName()); settled();
        require(directory.communityId().isEmpty() && directory.communities().isEmpty(), "member leaves without deleting other accounts");
        api.overrideRole.clear(); const auto retained = api.addSpace("Destructor fixture", "owner");
        const int beforeDestruction = api.held; const int closedBeforeDestruction = api.disconnected;
        {
            auto temporary = std::make_unique<ChannelDirectory>(&account);
            require(waitFor([&]() { return !temporary->busy() && !temporary->communities().isEmpty(); }), "temporary directory loads");
            api.holdChannels = true; temporary->chooseCommunity(retained);
            require(waitFor([&]() { return api.held > beforeDestruction; }), "temporary pending channel request");
        }
        require(waitFor([&]() { return api.disconnected > closedBeforeDestruction; }), "destruction aborts pending directory transport");
        account.signOut();
        std::cout << "Qt community create/join/admin, bounded replies, role recovery and cancellation passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
