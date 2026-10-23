#include "AccountSession.h"
#include "FriendsDirectory.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
    return condition();
}
QString uuid(int n) { return QStringLiteral("00000000-0000-0000-0000-%1").arg(n, 12, 16, QChar('0')); }
const QString self = uuid(1), peer = uuid(2), conversation = uuid(3), requestId = uuid(4);
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
struct Api {
    QTcpServer server;
    QJsonArray requests;
    bool friendship = false, blocked = false, opened = false;
    bool malformed = false, oversized = false, holdRequests = false, holdMutation = false, wrongOpen = false, holdRefresh = false;
    int mutations = 0, refreshes = 0, closedHeld = 0, ttl = 900;
    QPointer<QTcpSocket> held, refresh;
    QString token = QString(64, 'a'), lastAccess;
    QJsonObject person() const { return {{"user_id", peer}, {"handle", "friend_one"}, {"display_name", "Friend <b>literal</b>"}}; }
    QJsonObject pending(bool incoming) const {
        auto row = person(); row.insert("id", requestId); row.insert("sender_id", incoming ? peer : self); row.insert("receiver_id", incoming ? self : peer); return row;
    }
    QByteArray session() const { return json({{"access_token", token}, {"refresh_token", QString(64, 'b')}, {"expires_in", ttl},
        {"server_owner", false}, {"user", QJsonObject{{"id", self}, {"handle", "owner_one"}, {"display_name", "Owner"}}}}); }
    static void respond(QTcpSocket *socket, int status, const QByteArray &bytes = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
    void hold(QTcpSocket *socket) {
        held = socket; QObject::connect(socket, &QTcpSocket::disconnected, &server, [this]() { ++closedHeld; });
    }
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QString &path, const QJsonObject &body) {
        if (path == "/v1/auth/login") { respond(socket, 200, session()); return; }
        if (path == "/v1/auth/refresh") {
            ++refreshes; token = QString(64, 'c'); ttl = 900;
            if (holdRefresh) { refresh = socket; return; }
            respond(socket, 200, session()); return;
        }
        if (path == "/v1/auth/logout") { respond(socket, 204); return; }
        if (method == "GET") {
            QJsonArray rows;
            if (path == "/v1/friends" && friendship) rows.append(person());
            if (path == "/v1/blocks" && blocked) rows.append(person());
            if (path == "/v1/dms" && opened && friendship) rows.append(QJsonObject{{"id", conversation}, {"other_user_id", peer}});
            if (path == "/v1/friends/requests") {
                if (holdRequests) { hold(socket); return; }
                if (oversized) { respond(socket, 200, QByteArray(262145, 'x')); return; }
                if (malformed) { respond(socket, 200, "[{\"user_id\":\"foreign-private-detail\"}]"); return; }
                rows = requests;
            }
            respond(socket, 200, QJsonDocument(rows).toJson(QJsonDocument::Compact)); return;
        }
        ++mutations;
        if (path == "/v1/friends/requests" && method == "POST") {
            require(body.value("handle").toString() == "friend_one", "exact handle sent"); requests = {pending(false)};
            if (holdMutation) { hold(socket); return; }
            respond(socket, 201, json(pending(false))); return;
        }
        if (path.contains("/v1/friends/requests/")) {
            if (path.endsWith("/accept")) friendship = true;
            requests = {}; respond(socket, 204); return;
        }
        if (path == "/v1/dms") {
            opened = true;
            respond(socket, 200, json({{"id", conversation}, {"other_user_id", wrongOpen ? uuid(999) : peer}})); return;
        }
        if (path == "/v1/blocks" && method == "POST") { require(body.value("user_id").toString() == peer, "block exact known user"); blocked = true; friendship = false; requests = {}; respond(socket, 204); return; }
        if (path.startsWith("/v1/blocks/") && method == "DELETE") { blocked = false; respond(socket, 204); return; }
        if (path.startsWith("/v1/friends/") && method == "DELETE") { friendship = false; respond(socket, 204); return; }
        respond(socket, 404);
    }
    Api() {
        require(server.listen(QHostAddress::LocalHost), "fixture listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n"); if (end < 0 || socket->property("answered").toBool()) return;
                    int length = 0; QByteArray access;
                    for (const auto &line : bytes.left(end).split('\n')) {
                        const auto colon = line.indexOf(':'); if (colon < 0) continue;
                        const auto name = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
                        if (name == "content-length") length = value.toInt();
                        if (name == "authorization") access = value;
                    }
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto first = bytes.left(bytes.indexOf("\r\n")).split(' '); const auto path = QString::fromUtf8(first.value(1));
                    if (!path.startsWith("/v1/auth/")) {
                        lastAccess = QString::fromUtf8(access);
                        if (access != "Bearer " + token.toUtf8()) { respond(socket, 401); return; }
                    }
                    dispatch(socket, first.value(0), path, QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
                });
            }
        });
    }
    ~Api() { server.close(); for (auto *socket : server.findChildren<QTcpSocket *>()) { socket->disconnect(); socket->abort(); } }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        Api api; AccountSession account; FriendsDirectory social(&account, nullptr, {60000, 1000});
        account.signIn(api.origin(), "owner@example.test", "private-test-password");
        require(waitFor([&]() { return social.verified() && !social.busy(); }), "four initial social lists");
        require(account.handle() == "owner_one" && social.friends().isEmpty(), "own exact handle and empty friends");
        QQmlEngine engine; int warnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) { ++warnings; for (const auto &error : errors) std::cerr << error.toString().toStdString() << '\n'; });
        engine.rootContext()->setContextProperty("socialController", &social); engine.rootContext()->setContextProperty("accountController", &account);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nFriendsPanel { social: socialController; account: accountController; width: 780 }", QUrl::fromLocalFile(QStringLiteral(GCP_FRIENDS_QML_DIR) + "/FriendsHarness.qml"));
        const std::unique_ptr<QObject> panel(component.create());
        if (!panel) for (const auto &error : component.errors()) std::cerr << error.toString().toStdString() << '\n';
        require(panel != nullptr, "shipped FriendsPanel loads");
        QQuickWindow window; window.resize(820, 650); qobject_cast<QQuickItem *>(panel.get())->setParentItem(window.contentItem()); window.show();
        const auto field = [&](const char *name) { auto *item = panel->findChild<QObject *>(name); require(item, "QML control exists"); return item; };
        const auto click = [&](const char *name) { auto *item = field(name); require(item->property("enabled").toBool(), "QML button enabled"); require(QMetaObject::invokeMethod(item, "clicked"), "QML click"); };
        field("friendHandle")->setProperty("text", "INVALID"); click("friendSendRequest"); require(api.mutations == 0, "invalid handle never sent");
        field("friendHandle")->setProperty("text", "friend_one"); click("friendSendRequest");
        require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().size() == 1; }), "shipped send request");
        social.acceptRequest(requestId); require(!social.busy() && api.mutations == 1, "sender cannot accept outgoing request");
        social.cancelRequest(requestId); require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().isEmpty(); }), "cancel outgoing");
        api.requests = {api.pending(true)}; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().size() == 1; }), "incoming request");
        social.declineRequest(requestId); require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().isEmpty(); }), "decline incoming");
        api.requests = {api.pending(true)}; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().size() == 1; }), "second incoming request");
        social.acceptRequest(requestId); require(waitFor([&]() { return social.verified() && !social.busy() && social.friends().size() == 1; }), "accept and refresh friends");
        api.wrongOpen = true; social.openConversation(peer);
        require(waitFor([&]() { return !social.busy(); }) && social.conversationId().isEmpty() && !social.verified(), "mismatched open response cannot change peer scope");
        api.wrongOpen = false; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy(); }), "restore verified state");
        social.openConversation(peer); require(waitFor([&]() { return social.verified() && !social.busy() && social.conversationId() == conversation; }), "open private scope");
        require(QMetaObject::invokeMethod(panel.get(), "ask", Q_ARG(QVariant, "block"), Q_ARG(QVariant, peer)), "open block confirmation");
        api.friendship = false; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy() && social.conversationId().isEmpty(); }), "remote unfriend clears selected scope");
        const auto beforeStale = api.mutations;
        require(QMetaObject::invokeMethod(field("friendConfirmation"), "accept"), "accept stale confirmation");
        require(api.mutations == beforeStale, "removed relationship invalidates confirmation");
        api.friendship = true; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy(); }), "restore fixture friendship");
        social.chooseConversation(conversation); require(social.conversationId() == conversation, "choose existing conversation");
        require(QMetaObject::invokeMethod(panel.get(), "ask", Q_ARG(QVariant, "block"), Q_ARG(QVariant, peer)), "open block confirmation again");
        require(QMetaObject::invokeMethod(field("friendConfirmation"), "accept"), "confirm block");
        require(waitFor([&]() { return social.verified() && !social.busy() && social.blocks().size() == 1 && social.conversationId().isEmpty(); }), "block and purge selection");
        require(QMetaObject::invokeMethod(panel.get(), "ask", Q_ARG(QVariant, "unblock"), Q_ARG(QVariant, peer)), "open unblock");
        require(QMetaObject::invokeMethod(field("friendConfirmation"), "accept"), "confirm unblock");
        require(waitFor([&]() { return social.verified() && !social.busy() && social.blocks().isEmpty() && social.friends().isEmpty(); }), "unblock leaves friendship absent");
        api.friendship = true; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy() && social.friends().size() == 1; }), "prepare atomic refresh");
        api.friendship = false; api.malformed = true; social.refresh();
        require(waitFor([&]() { return !social.busy(); }) && !social.verified() && social.friends().size() == 1 && !social.status().contains("foreign-private-detail"), "malformed later list cannot publish partial refresh");
        api.malformed = false; api.oversized = true; social.refresh();
        require(waitFor([&]() { return !social.busy(); }) && !social.verified(), "oversized friend response rejected");
        api.oversized = false; social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy(); }), "recover from invalid snapshots");
        api.holdMutation = true; social.sendRequest("friend_one"); require(waitFor([&]() { return api.held && social.mutating(); }), "held social write");
        const auto beforeCancel = api.mutations; social.cancel(); api.holdMutation = false;
        require(waitFor([&]() { return api.closedHeld > 0; }), "cancel aborts pending transport");
        social.refresh(); require(waitFor([&]() { return social.verified() && !social.busy() && social.requests().size() == 1; }), "refresh recovers committed uncertain request");
        require(api.mutations == beforeCancel, "cancelled mutation was not automatically repeated");
        api.holdRequests = true;
        api.held = nullptr;
        auto temporary = std::make_unique<FriendsDirectory>(&account, nullptr, FriendsRuntime{60000, 1000});
        require(waitFor([&]() { return api.held && temporary->busy(); }), "temporary pending directory");
        const auto closedBefore = api.closedHeld; temporary.reset();
        require(waitFor([&]() { return api.closedHeld > closedBefore; }), "destruction aborts pending request");
        api.holdRequests = false;
        require(QMetaObject::invokeMethod(panel.get(), "ask", Q_ARG(QVariant, "block"), Q_ARG(QVariant, peer)), "confirmation before logout");
        account.signOut(); require(social.requests().isEmpty() && social.friends().isEmpty() && social.conversationId().isEmpty() && account.handle().isEmpty(), "logout erases social scope");
        require(panel->property("action").toString().isEmpty(), "logout clears social confirmation");
        require(warnings == 0, "no shipped FriendsPanel warnings");

        Api rotating; rotating.ttl = 61; rotating.holdRefresh = true; rotating.friendship = true; rotating.opened = true;
        AccountSession account2; FriendsDirectory social2(&account2, nullptr, {60000, 2000});
        account2.signIn(rotating.origin(), "owner@example.test", "private-test-password");
        require(waitFor([&]() { return social2.verified() && !social2.busy(); }), "short-lived social session");
        social2.chooseConversation(conversation); rotating.holdRequests = true; social2.refresh();
        require(waitFor([&]() { return rotating.held && social2.busy(); }), "old-token social read held");
        require(waitFor([&]() { return rotating.refreshes == 1 && account2.busy(); }), "real social account refresh timer");
        Api::respond(rotating.held, 401); rotating.holdRequests = false;
        require(waitFor([&]() { return !social2.busy(); }) && social2.conversationId() == conversation, "expected rotation 401 retains private selection");
        Api::respond(rotating.refresh, 200, rotating.session());
        require(waitFor([&]() { return social2.verified() && !social2.busy() && rotating.lastAccess == "Bearer " + rotating.token; }), "fresh-token social snapshot");
        require(social2.conversationId() == conversation && account2.handle() == "owner_one", "rotation preserves peer scope and handle");
        std::cout << "Friends QML, direction guards, snapshots, cancellation, bounds and in-flight token rotation passed.\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
