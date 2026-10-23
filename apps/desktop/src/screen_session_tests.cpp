#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ScreenSession.h"
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()>& condition, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
    return condition();
}
const QString community = QStringLiteral("00000000-0000-0000-0000-000000000001");
const QString channel = QStringLiteral("00000000-0000-0000-0000-000000000002");
const QString lease = QStringLiteral("00000000-0000-0000-0000-000000000003");
struct Api {
    QTcpServer server;
    int creates = 0, renews = 0, deletes = 0, deny = 0, ttl = 60;
    bool hold = false, oversized = false;
    QString malformed;
    QList<QPointer<QTcpSocket>> held;
    static void respond(QTcpSocket* socket, int code, const QByteArray& body) {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QByteArray grant() const {
        if (oversized) return QByteArray(18000, 'x');
        const auto identity = QStringLiteral("gcp-screen-publish-") + lease;
        const auto room = QStringLiteral("gcp-screen-") + channel;
        auto sources = QJsonArray{malformed == "sources" ? "microphone" : "screen_share"};
        auto token = QByteArrayLiteral("eyJhbGciOiJIUzI1NiJ9.") + QJsonDocument(QJsonObject{
            {"sub", identity}, {"video", QJsonObject{{"room", room}, {"roomJoin", true}, {"canPublish", true},
                {"canSubscribe", malformed == "subscribe"}, {"canPublishData", false}, {"canPublishSources", sources}}}
        }).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) + ".fixture_signature";
        QJsonObject json{{"id", lease}, {"purpose", "publish"}, {"participant_identity", identity}, {"room", room},
            {"server_url", "wss://screen.invalid"}, {"participant_token", QString::fromLatin1(token)}, {"expires_in", ttl}};
        if (malformed == "identity") json["participant_identity"] = "gcp-session-other";
        if (malformed == "room") json["room"] = "gcp-voice-other";
        if (malformed == "purpose") json["purpose"] = "view";
        if (malformed == "url") json["server_url"] = "ws://external.invalid";
        if (malformed == "fraction") json["expires_in"] = 2.5;
        return QJsonDocument(json).toJson(QJsonDocument::Compact);
    }
    Api() {
        require(server.listen(QHostAddress::LocalHost), "fixture HTTP listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", bytes);
                    if (socket->property("answered").toBool() || !bytes.contains("\r\n\r\n")) return;
                    // Wait for the complete POST body, not just the headers.
                    const auto headerEnd = bytes.indexOf("\r\n\r\n");
                    int length = 0;
                    for (auto line : bytes.left(headerEnd).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < headerEnd + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto path = bytes.split(' ').value(1); const auto method = bytes.split(' ').value(0);
                    QByteArray body; int code = 200;
                    if (path == "/v1/auth/login") body = QJsonDocument(QJsonObject{{"access_token", QString(64, 'a')}, {"refresh_token", QString(64, 'b')}, {"expires_in", 900}, {"user", QJsonObject{{"id", "00000000-0000-0000-0000-000000000010"}, {"display_name", "Test"}}}}).toJson();
                    else if (path == "/v1/auth/logout") code = 204;
                    else if (path == "/v1/communities") body = "[{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}]";
                    else if (path == "/v1/communities/" + community.toUtf8()) body = "{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}";
                    else if (path.endsWith("/channels")) body = "[{\"id\":\"" + channel.toUtf8() + "\",\"name\":\"team\",\"channel_kind\":\"voice\",\"position\":0}]";
                    else if (method == "DELETE" && path.endsWith("/" + lease.toUtf8())) { ++deletes; code = 204; }
                    else if (path.endsWith("/screen-sessions") || path.endsWith("/renew")) {
                        require(bytes.mid(headerEnd + 4).contains("\"consent\":true"), "explicit consent in real HTTP request");
                        if (path.endsWith("/renew")) ++renews; else ++creates;
                        if (hold) { held.append(socket); return; }
                        code = deny ? deny : path.endsWith("/renew") ? 200 : 201; body = grant();
                    } else code = 404;
                    respond(socket, code, body);
                });
            }
        });
    }
    ~Api() { server.close(); for (auto* socket : server.findChildren<QTcpSocket*>()) { socket->disconnect(); socket->abort(); } }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        for (const auto* key : {"DATABASE_URL", "LIVEKIT_API_KEY", "LiVeKiT_ApI_SeCrEt", "LIVEKIT_TOKEN", "LIVEKIT_URL", "GCP_MEDIA_SECRET", "GCP_ACCESS_TOKEN", "GCP_REFRESH_TOKEN"}) qputenv(key, "dummy-sensitive-value");
        QTemporaryFile log; require(log.open(), "fixture trace"); qputenv("GCP_SCREEN_FIXTURE_LOG", log.fileName().toUtf8());
        Api api; AccountSession account; ChannelDirectory directory(&account);
        const auto program = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-screen-process-fixture.exe"));
        ScreenSession screen(&account, &directory, nullptr, {program, 200, 700, 1000});
        QQmlEngine engine; int warnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError>&) { ++warnings; });
        engine.rootContext()->setContextProperty("controller", &screen);
        engine.rootContext()->setContextProperty("rooms", &directory);
        engine.rootContext()->setContextProperty("login", &account);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nScreenPanel { screen: controller; directory: rooms; account: login; width: 620 }", QUrl::fromLocalFile(QStringLiteral(GCP_SCREEN_QML_DIR) + "/ScreenHarness.qml"));
        const std::unique_ptr<QObject> panel(component.create());
        if (!panel) std::cerr << component.errorString().toStdString();
        require(panel != nullptr, "actual ScreenPanel loads");
        QQuickWindow window; window.resize(660, 700); qobject_cast<QQuickItem*>(panel.get())->setParentItem(window.contentItem()); window.show();
        const auto field = [&](const char* name) { auto* value = panel->findChild<QObject*>(name); require(value != nullptr, "screen QML control exists"); return value; };
        const auto click = [&](const char* name) { require(field(name)->property("enabled").toBool(), "screen QML action enabled"); require(QMetaObject::invokeMethod(field(name), "clicked"), "screen QML button action"); };
        require(!field("screenChoose")->property("enabled").toBool(), "cannot select source while signed out");
        screen.chooseSource(); require(!screen.busy() && api.creates == 0, "signed out no child or lease");
        const auto login = [&] {
            account.signIn(api.origin(), "test@example.test", "password");
            require(waitFor([&] { return account.signedIn() && !directory.busy() && !directory.communities().isEmpty(); }), "account login and community list");
            directory.chooseCommunity(community);
            require(waitFor([&] { return !directory.busy() && !directory.channels().isEmpty(); }), "voice directory loaded");
            directory.chooseVoiceChannel(channel);
        };
        login();
        const auto pick = [&] { click("screenChoose"); require(waitFor([&] { return screen.awaitingConsent(); }), "source selected without capture"); };
        const auto confirm = [&] { field("screenConsent")->setProperty("checked", true); click("screenConfirm"); };
        const auto stopped = [&] { require(waitFor([&] { return !screen.busy(); }), "bounded child teardown and lease cleanup"); require(!screen.sharing() && !screen.awaitingConsent(), "stopped consent and indicator cleared"); };
        pick(); require(api.creates == 0 && !screen.sharing() && screen.sourceName() == "<b>Window</b>" && !field("screenConfirm")->property("enabled").toBool(), "picker alone never authorizes capture; plain source metadata");
        click("screenCancel"); stopped(); require(api.creates == 0, "cancel has no server side effect");
        pick(); screen.confirmShare(false); stopped(); require(api.creates == 0, "C++ consent gate also refuses false");
        pick(); confirm(); require(waitFor([&] { return screen.sharing() && api.renews >= 2; }), "grant and renewed publication protocol");
        click("screenStop"); stopped(); require(api.deletes == 1 && directory.channelId() == channel, "stop revokes own lease without changing voice scope");
        for (const auto& malformed : {"sources", "subscribe", "identity", "room", "purpose", "url", "fraction"}) {
            api.malformed = malformed; pick(); confirm(); stopped(); require(!screen.sharing(), "wrong role, scope, origin or lifetime never reaches publisher");
        }
        api.malformed.clear(); api.oversized = true; pick(); confirm(); stopped(); api.oversized = false;
        api.hold = true; pick(); confirm(); require(waitFor([&] { return !api.held.isEmpty(); }), "create reply held in flight");
        screen.stop(); stopped();
        for (auto socket : api.held) if (socket && socket->state() == QAbstractSocket::ConnectedState) Api::respond(socket, 201, api.grant());
        api.held.clear(); api.hold = false;
        require(!screen.sharing() && !screen.awaitingConsent(), "late grant cannot restore consent");
        pick(); confirm(); require(waitFor([&] { return screen.sharing(); }), "restart requires a fresh picker and consent");
        api.deny = 403; stopped(); require(screen.status().contains("access"), "renewal revocation stops local capture"); api.deny = 0;
        api.ttl = 1; api.hold = false; pick(); confirm(); require(waitFor([&] { return screen.sharing(); }), "short live lease");
        api.hold = true; stopped(); api.hold = false; api.ttl = 60; api.held.clear();
        for (const auto* mode : {"cancel", "flood"}) { qputenv("GCP_SCREEN_FIXTURE_MODE", mode); click("screenChoose"); stopped(); }
        qputenv("GCP_SCREEN_FIXTURE_MODE", "closed_source"); pick(); stopped();
        for (const auto* mode : {"interrupted", "stopped_alive", "no_frame"}) { qputenv("GCP_SCREEN_FIXTURE_MODE", mode); pick(); confirm(); stopped(); }
        qputenv("GCP_SCREEN_FIXTURE_MODE", "hang"); pick(); confirm(); require(waitFor([&] { return screen.sharing(); }), "hanging child starts"); screen.stop(); stopped();
        qunsetenv("GCP_SCREEN_FIXTURE_MODE");
        pick(); confirm(); require(waitFor([&] { return screen.sharing(); }), "channel change setup"); directory.chooseVoiceChannel({}); stopped();
        directory.chooseVoiceChannel(channel); pick(); confirm(); require(waitFor([&] { return screen.sharing(); }), "logout setup"); account.signOut(); stopped();
        require(warnings == 0, "ScreenPanel has no QML warnings");
        std::cout << "Screen Qt/QML HTTP/subprocess consent, lease, cancellation, expiry, revocation, output limits and kill watchdog passed. No actual capture or media tested.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
