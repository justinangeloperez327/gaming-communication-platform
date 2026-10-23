#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatSession.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QMap>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrlQuery>
#include <QWebSocketServer>
#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition, int timeout = 5000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2);
    }
    return condition();
}
QString uuid(int n) { return QStringLiteral("00000000-0000-0000-0000-%1").arg(n, 12, 16, QChar('0')); }
const QString space = uuid(1), text = uuid(2), otherText = uuid(3), voice = uuid(4), self = uuid(700), friendId = uuid(701);
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject message(int n, const QString &body, const QString &author = friendId) {
    // Deliberately share a millisecond: the API's microsecond ordering matters.
    const qint64 created = 1700000000000000LL + n;
    return {{"id", uuid(n)}, {"channel_id", text}, {"author_id", author}, {"body", body},
        {"created_at_us", created}, {"created_at_ms", created / 1000},
        {"edited_at_ms", QJsonValue::Null}, {"deleted_at_ms", QJsonValue::Null}};
}
// Real loopback HTTP and WebSockets with controlled loss/malformed replies.
// PostgreSQL and actual Rust interoperability are checked by the separate API job.
struct Api {
    QTcpServer http;
    QWebSocketServer ws{QStringLiteral("chat fixture"), QWebSocketServer::NonSecureMode};
    QList<QPointer<QWebSocket>> peers;
    QMap<QString, QJsonObject> records;
    QPointer<QTcpSocket> held, refresh;
    QByteArray heldBody;
    QString token = QString(64, 'a');
    bool offline = false, holdPage = false, lostPost = false, rejectPost = false, deny = false, malformed = false, oversized = false, holdRefresh = false;
    int posts = 0, reads = 0, connections = 0, refreshes = 0, ttl = 900;
    QStringList attempts, headers;
    static void respond(QTcpSocket *socket, int status, const QByteArray &body = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QByteArray session() const {
        return json({{"access_token", token}, {"refresh_token", QString(64, 'b')}, {"expires_in", ttl},
            {"server_owner", false}, {"user", QJsonObject{{"id", self}, {"display_name", "Owner"}}}});
    }
    void announce(const QString &kind, const QString &id) {
        for (auto &peer : peers) if (peer && peer->state() == QAbstractSocket::ConnectedState)
            peer->sendTextMessage(QString::fromUtf8(json({{"type", kind}, {"channel_id", text},
                {"user_id", friendId}, {"message_id", id}})));
    }
    void disconnectPeers() { for (auto &peer : peers) if (peer) peer->abort(); }
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QUrl &url, const QJsonObject &body) {
        const auto path = url.path();
        if (path == "/v1/auth/login") { respond(socket, 200, session()); return; }
        if (path == "/v1/auth/refresh") {
            ++refreshes; token = QString(64, 'c'); ttl = 900;
            if (holdRefresh) { refresh = socket; return; }
            respond(socket, 200, session()); return;
        }
        if (path == "/v1/auth/logout") { respond(socket, 204); return; }
        if (path == "/v1/communities") {
            respond(socket, 200, deny ? QByteArray("[]") : QJsonDocument(QJsonArray{QJsonObject{{"id", space}, {"name", "Friends"}, {"role", "owner"}}}).toJson()); return;
        }
        if (deny) { respond(socket, 404); return; }
        if (path == "/v1/communities/" + space) {
            respond(socket, 200, json({{"id", space}, {"name", "Friends"}, {"role", "owner"}})); return;
        }
        if (path.endsWith("/channels")) {
            respond(socket, 200, QJsonDocument(QJsonArray{
                QJsonObject{{"id", text}, {"name", "general"}, {"channel_kind", "text"}, {"position", 0}},
                QJsonObject{{"id", otherText}, {"name", "games"}, {"channel_kind", "text"}, {"position", 1}},
                QJsonObject{{"id", voice}, {"name", "team"}, {"channel_kind", "voice"}, {"position", 2}}}).toJson()); return;
        }
        if (path.endsWith("/members")) {
            respond(socket, 200, QJsonDocument(QJsonArray{QJsonObject{{"user_id", self}, {"display_name", "Owner"}},
                QJsonObject{{"user_id", friendId}, {"display_name", "Friend <b>plain</b>"}}}).toJson()); return;
        }
        if (path.endsWith("/messages") && method == "GET") {
            ++reads;
            if (oversized) { respond(socket, 200, QByteArray(524289, 'x')); return; }
            if (malformed) { respond(socket, 200, "{\"messages\":[{}],\"next_before\":null}"); return; }
            QList<QJsonObject> rows;
            if (!path.contains(otherText)) for (const auto &row : records) rows.append(row);
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
                return a.value("created_at_us").toInteger() == b.value("created_at_us").toInteger() ?
                    a.value("id").toString() > b.value("id").toString() : a.value("created_at_us").toInteger() > b.value("created_at_us").toInteger();
            });
            const auto before = QUrlQuery(url).queryItemValue("before");
            if (!before.isEmpty()) {
                const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto &r) { return r.value("id") == before; });
                rows.erase(rows.begin(), at == rows.end() ? at : std::next(at));
            }
            QJsonArray page; for (int i = 0; i < qMin(50, static_cast<int>(rows.size())); ++i) page.append(rows[i]);
            const auto bytes = json({{"messages", page}, {"next_before", rows.size() > 50 ? QJsonValue(page.last().toObject().value("id")) : QJsonValue::Null}});
            if (holdPage) { held = socket; heldBody = bytes; return; }
            respond(socket, 200, bytes); return;
        }
        if (path.endsWith("/messages") && method == "POST") {
            ++posts; const auto id = body.value("client_message_id").toString(); attempts.append(id);
            if (rejectPost) { respond(socket, 503); return; }
            const bool exists = records.contains(id);
            if (!exists) {
                auto row = message(1000 + posts, body.value("body").toString(), self);
                row.insert("id", id); records.insert(id, row);
            }
            // The write committed but its result is unreadable. A bare TCP
            // close can be retried internally by Qt before the app sees it.
            if (lostPost) { lostPost = false; respond(socket, 201, "{"); return; }
            respond(socket, exists ? 200 : 201, json(records[id])); return;
        }
        const auto id = path.section('/', -1);
        if (records.contains(id) && path.contains("/messages/")) {
            auto &row = records[id];
            if (method == "PATCH") { row.insert("body", body.value("body")); row.insert("edited_at_ms", 1700000000010LL); }
            if (method == "DELETE") { row.insert("body", QJsonValue::Null); row.insert("deleted_at_ms", 1700000000011LL); respond(socket, 204); return; }
            respond(socket, 200, json(row)); return;
        }
        respond(socket, 404);
    }
    Api() {
        require(http.listen(QHostAddress::LocalHost), "fixture listen");
        QObject::connect(&ws, &QWebSocketServer::newConnection, &http, [this]() {
            auto *peer = ws.nextPendingConnection(); peers.append(peer); ++connections;
            QObject::connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
            const auto ch = peer->requestUrl().path().contains(otherText) ? otherText : text;
            peer->sendTextMessage(QString::fromUtf8(json({{"type", "ready"}, {"channel_id", ch}})));
        });
        QObject::connect(&http, &QTcpServer::newConnection, &http, [this]() {
            while (http.hasPendingConnections()) {
                auto *socket = http.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    const auto bytes = socket->peek(32768); const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("answered").toBool()) return;
                    QByteArray auth; bool upgrade = false; int length = 0;
                    for (const auto &line : bytes.left(end).split('\n')) {
                        const auto colon = line.indexOf(':'); if (colon < 0) continue;
                        const auto name = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
                        if (name == "authorization") auth = value;
                        if (name == "upgrade") upgrade = value.toLower() == "websocket";
                        if (name == "content-length") length = value.toInt();
                    }
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto first = bytes.left(bytes.indexOf("\r\n")).split(' ');
                    const QUrl url(QString::fromUtf8(first.value(1)));
                    if (upgrade) {
                        headers.append(QString::fromUtf8(auth));
                        require(!url.hasQuery(), "no token in socket URL");
                        if (offline || auth != "Bearer " + token.toUtf8()) { respond(socket, 401); return; }
                        QObject::disconnect(socket, nullptr, socket, nullptr);
                        ws.handleConnection(socket); return;
                    }
                    socket->read(end + 4 + length);
                    if (!url.path().startsWith("/v1/auth/") && auth != "Bearer " + token.toUtf8()) { respond(socket, 401); return; }
                    dispatch(socket, first.value(0), url, QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
                });
            }
        });
    }
    ~Api() {
        http.close();
        for (auto &peer : peers) if (peer) { peer->disconnect(); peer->abort(); delete peer.data(); }
        for (auto *socket : http.findChildren<QTcpSocket *>()) { socket->disconnect(); socket->abort(); }
    }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(http.serverPort()); }
};
void choose(AccountSession &account, ChannelDirectory &directory, Api &api) {
    account.signIn(api.origin(), "owner@example.test", "test-password-123");
    require(waitFor([&]() { return !directory.busy() && !directory.communities().isEmpty(); }), "login/directory");
    directory.chooseCommunity(space);
    require(waitFor([&]() { return !directory.busy() && directory.allChannels().size() == 3; }), "channel list");
    require(directory.textChannelId().isEmpty(), "listing does not select text");
    directory.chooseTextChannel(text);
}
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        Api api;
        for (int i = 10; i < 230; ++i) api.records.insert(uuid(i), message(i, QString::number(i)));
        AccountSession account; ChannelDirectory directory(&account);
        ChatSession chat(&account, &directory, nullptr, {50, 60000, 1000});
        choose(account, directory, api);
        auto *model = static_cast<MessageModel *>(chat.messages());
        require(waitFor([&]() { return chat.live() && chat.canMutate() && !chat.loading(); }), "HTTP and WebSocket ready");
        require(model->rowCount() == 50 && model->rows().first().id == uuid(180), "latest history chronological with microsecond precision");
        require(model->data(model->index(0), MessageModel::AuthorName).toString() == "Friend <b>plain</b>", "member display names");
        require(api.headers.last() == "Bearer " + account.accessToken(), "authenticated socket header");
        directory.chooseVoiceChannel(voice); int voiceChanges = 0;
        QObject::connect(&directory, &ChannelDirectory::selectionChanged, &chat, [&]() { ++voiceChanges; });
        directory.chooseTextChannel(otherText);
        require(waitFor([&]() { return chat.canMutate() && !chat.loading() && model->rowCount() == 0; }), "text switch");
        require(directory.channelId() == voice && voiceChanges == 0, "text selection is independent of voice");
        directory.chooseTextChannel(text);
        require(waitFor([&]() { return chat.canMutate() && !chat.loading() && model->rowCount() == 50; }), "return to history");
        for (int count : {100, 150, 200, 200}) {
            chat.loadOlder(); require(waitFor([&]() { return !chat.loading(); }), "older response");
            require(model->rowCount() == count, "bounded pagination window");
        }
        require(!chat.hasOlder() && model->rows().first().id == uuid(10), "pagination reaches oldest without skipping");
        api.records.insert(uuid(500), message(500, "new while browsing")); api.announce("message_created", uuid(500));
        require(waitFor([&]() { return chat.newerAvailable(); }) && !model->find(uuid(500)), "older window is not silently moved by live messages");
        chat.refreshLatest(); require(waitFor([&]() { return !chat.loading() && !chat.browsingOlder() && model->find(uuid(500)).has_value(); }), "back to latest");

        QQmlEngine engine; int warnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors) std::cerr << error.toString().toStdString() << '\n'; ++warnings;
        });
        engine.rootContext()->setContextProperty("chatController", &chat);
        engine.rootContext()->setContextProperty("directoryController", &directory);
        engine.rootContext()->setContextProperty("accountController", &account);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nChatPanel { chat: chatController; directory: directoryController; account: accountController; width: 700 }",
            QUrl::fromLocalFile(QStringLiteral(GCP_CHAT_QML_DIR) + "/ChatHarness.qml"));
        const std::unique_ptr<QObject> panel(component.create());
        if (!panel) for (const auto &error : component.errors()) std::cerr << error.toString().toStdString() << '\n';
        require(panel != nullptr, "shipped ChatPanel loads");
        QQuickWindow window; window.resize(750, 850); qobject_cast<QQuickItem *>(panel.get())->setParentItem(window.contentItem()); window.show();
        const auto field = [&](const char *name) { auto *item = panel->findChild<QObject *>(name); require(item, "QML field"); return item; };
        const auto click = [&](const char *name) { auto *item = field(name); require(item->property("enabled").toBool(), "button enabled"); require(QMetaObject::invokeMethod(item, "clicked"), "click"); };
        field("chatComposer")->setProperty("text", QString(2001, QChar(0x00e9)));
        require(chat.draftBytes() == 4002 && !chat.canSubmit(), "UTF8 limit, not character count");
        field("chatComposer")->setProperty("text", "<b>plain</b> & hello\nfriend");
        api.lostPost = true; click("chatSend"); chat.sendDraft();
        require(waitFor([&]() { return !chat.sending(); }) && api.posts == 1 && chat.retrySend() && !chat.draft().isEmpty(), "ambiguous response keeps draft and one attempt");
        require(!chat.setDraft("different body"), "ambiguous send cannot reuse its ID for changed content");
        click("chatSend"); require(waitFor([&]() { return !chat.sending() && chat.draft().isEmpty(); }), "explicit idempotent retry");
        require(api.posts == 2 && api.attempts[0] == api.attempts[1], "retry preserves stable message ID");
        const auto sent = api.attempts.last();
        require(model->find(sent)->body == "<b>plain</b> & hello\nfriend", "literal text preserved");
        require(QMetaObject::invokeMethod(panel.get(), "beginEdit", Q_ARG(QVariant, sent), Q_ARG(QVariant, "edited")), "open edit");
        click("chatSaveEdit"); require(waitFor([&]() { return !chat.sending() && model->find(sent)->body == "edited"; }), "shipped edit submits");
        require(QMetaObject::invokeMethod(panel.get(), "askDelete", Q_ARG(QVariant, sent)), "open delete confirmation");
        require(QMetaObject::invokeMethod(field("chatDeleteConfirmation"), "accept"), "confirm delete");
        require(waitFor([&]() { return model->find(sent)->deletedMs != 0; }), "delete fetches server tombstone");
        api.rejectPost = true; chat.setDraft("server never committed this send"); click("chatSend");
        require(waitFor([&]() { return !chat.sending(); }) && chat.retrySend() && !api.records.contains(api.attempts.last()), "uncommitted send retains explicit retry");
        const auto readsBeforeRetryRefresh = api.reads, postsBeforeRetryRefresh = api.posts;
        chat.refreshLatest();
        require(waitFor([&]() { return api.reads > readsBeforeRetryRefresh && !chat.loading(); }), "refresh after uncommitted send");
        waitFor([]() { return false; }, 400); // Observe several coalescing intervals.
        require(api.reads <= readsBeforeRetryRefresh + 2 && api.posts == postsBeforeRetryRefresh && chat.retrySend(), "missing send ID cannot create a resync/request loop or automatic write");
        chat.clearDraft(); api.rejectPost = false;
        chat.editMessage(uuid(500), "forbidden"); require(!chat.sending(), "cannot edit another author's message");

        auto changed = api.records[uuid(500)]; changed.insert("body", "live edit"); changed.insert("edited_at_ms", 1700000000020LL);
        api.records[uuid(500)] = changed; api.announce("message_edited", uuid(500));
        require(waitFor([&]() { return model->find(uuid(500))->body == "live edit"; }), "live event refetches authorized message");
        api.offline = true; api.disconnectPeers();
        require(waitFor([&]() { return !chat.live(); }), "socket disconnect observed");
        changed.insert("body", QJsonValue::Null); changed.insert("deleted_at_ms", 1700000000021LL); api.records[uuid(500)] = changed;
        api.records.insert(uuid(501), message(501, "missed creation")); api.offline = false;
        require(waitFor([&]() { return chat.live() && model->find(uuid(501)) && model->find(uuid(500))->deletedMs; }), "reconnect snapshot recovers missed deletion and creation");
        for (auto &peer : api.peers) if (peer) peer->sendTextMessage("{\"type\":\"resync_required\"}");
        const auto connections = api.connections;
        require(waitFor([&]() { return api.connections > connections && chat.live() && !chat.loading(); }), "gap notification forces resubscribe and snapshot");
        api.holdPage = true; chat.refreshLatest(); require(waitFor([&]() { return !api.held.isNull(); }), "held history response");
        directory.chooseTextChannel(otherText); api.holdPage = false;
        if (api.held && api.held->state() == QAbstractSocket::ConnectedState) Api::respond(api.held, 200, api.heldBody);
        require(waitFor([&]() { return chat.canMutate() && !chat.loading() && model->rowCount() == 0; }), "stale channel response cancelled");
        directory.chooseTextChannel(text); require(waitFor([&]() { return chat.canMutate() && !chat.loading(); }), "restore channel");
        chat.setDraft("retained on malformed history"); api.malformed = true; chat.refreshLatest();
        require(waitFor([&]() { return !chat.loading() && !chat.canMutate(); }) && !chat.draft().isEmpty(), "malformed page cannot authorize writes");
        api.malformed = false; api.oversized = true; const auto readsBeforeOversize = api.reads; chat.refreshLatest();
        require(waitFor([&]() { return api.reads > readsBeforeOversize && !chat.loading() && !chat.canMutate(); }), "oversized history rejected");
        api.oversized = false; chat.refreshLatest(); require(waitFor([&]() { return chat.canMutate() && !chat.loading(); }), "manual recovery");
        api.deny = true; chat.refreshLatest(); require(waitFor([&]() { return !chat.loading() && model->rowCount() == 0 && chat.draft().isEmpty(); }), "membership loss purges history and affected drafts");
        account.signOut(); require(!chat.canSubmit() && chat.draft().isEmpty(), "logout clears messaging state");
        require(warnings == 0, "no shipped QML warnings");

        // Exercise the actual refresh timer and replacement Authorization header.
        Api rotating; rotating.ttl = 61; rotating.holdRefresh = true;
        AccountSession account2; ChannelDirectory directory2(&account2);
        ChatSession chat2(&account2, &directory2, nullptr, {50, 60000, 2000});
        choose(account2, directory2, rotating);
        require(waitFor([&]() { return chat2.live() && chat2.canMutate() && !chat2.loading(); }), "initial short-lived session");
        chat2.setDraft("keep across rotation");
        rotating.holdPage = true; chat2.refreshLatest();
        require(waitFor([&]() { return rotating.held && chat2.loading(); }), "old-token history in flight before rotation");
        require(waitFor([&]() { return rotating.refreshes == 1 && account2.busy(); }), "real account refresh timer");
        // Simulate the actual server's atomic old-token invalidation while the
        // replacement token's response has not reached AccountSession yet.
        Api::respond(rotating.held, 401); rotating.holdPage = false;
        require(waitFor([&]() { return !chat2.loading(); }), "read during refresh");
        require(chat2.draft() == "keep across rotation", "expected refresh 401 does not purge draft");
        Api::respond(rotating.refresh, 200, rotating.session()); rotating.holdRefresh = false;
        require(waitFor([&]() { return chat2.live() && chat2.canMutate() && rotating.headers.last() == "Bearer " + rotating.token; }), "socket replaced after token rotation");
        require(chat2.draft() == "keep across rotation" && account2.userId() == self, "rotation preserves draft and identity");
        std::cout << "Desktop text protocol, QML, loss recovery, bounds and token rotation passed (loopback fixtures).\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
