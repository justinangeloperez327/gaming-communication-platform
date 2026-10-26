#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ScreenViewer.h"
#include "ScreenVideoItem.h"
#include <qqml.h>
#include <cstdio>
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
    int creates = 0, renews = 0, deletes = 0, deny = 0, ttl = 60, targets = 0;
    bool hold = false, oversized = false;
    QString malformed;
    int loginTtl = 900, refreshes = 0;
    bool holdRefresh = false;
    QString access = QString(64, 'a'), lastLeaseAccess;
    QPointer<QTcpSocket> refreshReply;
    QByteArray session(int ttl) const {
        return QJsonDocument(QJsonObject{{"access_token", access}, {"refresh_token", QString(64, 'b')}, {"expires_in", ttl}, {"user", QJsonObject{{"id", "00000000-0000-0000-0000-000000000010"}, {"display_name", "Test"}}}}).toJson();
    }
    void finishRefresh() {
        access = QString(64, 'c'); holdRefresh = false;
        if (refreshReply) respond(refreshReply, 200, session(900));
        refreshReply = nullptr;
    }
    QList<QPointer<QTcpSocket>> held;
    static void respond(QTcpSocket* socket, int code, const QByteArray& body) {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QByteArray grant() const {
        if (oversized) return QByteArray(18000, 'x');
        const auto identity = QStringLiteral("gcp-screen-view-") + lease;
        const auto room = QStringLiteral("gcp-screen-") + channel;
        auto sources = malformed == "sources" ? QJsonArray{"screen_share"} : QJsonArray{};
        auto token = QByteArrayLiteral("eyJhbGciOiJIUzI1NiJ9.") + QJsonDocument(QJsonObject{
            {"sub", identity}, {"video", QJsonObject{{"room", room}, {"roomJoin", true}, {"canPublish", malformed == "publish"},
                {"canSubscribe", malformed != "subscribe"}, {"canPublishData", false}, {"canPublishSources", sources}}}
        }).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) + ".fixture_signature";
        QJsonObject json{{"id", lease}, {"purpose", "view"}, {"participant_identity", identity}, {"room", room},
            {"server_url", "wss://screen.invalid"}, {"participant_token", QString::fromLatin1(token)}, {"expires_in", ttl}};
        if (malformed == "identity") json["participant_identity"] = "gcp-session-other";
        if (malformed == "room") json["room"] = "gcp-voice-other";
        if (malformed == "purpose") json["purpose"] = "publish";
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
                    if (path == "/v1/auth/login") body = session(loginTtl);
                    else if (path == "/v1/auth/refresh") {
                        ++refreshes;
                        if (holdRefresh) { refreshReply = socket; return; }
                        access = QString(64, 'c'); body = session(900);
                    }
                    else if (path == "/v1/auth/logout") code = 204;
                    else if (!bytes.toLower().contains("authorization: bearer " + access.toUtf8())) code = 401;
                    else if (path == "/v1/communities") body = "[{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}]";
                    else if (path == "/v1/communities/" + community.toUtf8()) body = "{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}";
                    else if (path.endsWith("/channels")) body = QJsonDocument(QJsonArray{
                        QJsonObject{{"id", channel}, {"name", "team"}, {"channel_kind", "voice"}, {"position", 0}},
                        QJsonObject{{"id", "00000000-0000-0000-0000-000000000004"}, {"name", "other"}, {"channel_kind", "voice"}, {"position", 1}}}).toJson();
                    else if (method == "GET" && path.endsWith("/screen-sessions")) {
                        body = QJsonDocument(QJsonArray{QJsonObject{{"id", "00000000-0000-0000-0000-000000000005"}, {"user_id", "00000000-0000-0000-0000-000000000020"}, {"display_name", "<b>Friend</b>"}, {"participant_identity", "gcp-screen-publish-00000000-0000-0000-0000-000000000005"}}}).toJson();
                    }
                    else if (path.endsWith("/watch")) {
                        ++targets; code = deny ? deny : 200;
                        body = QJsonDocument(QJsonObject{{"participant_identity", "gcp-screen-publish-00000000-0000-0000-0000-000000000005"}, {"track_sid", malformed == "track" ? "invalid" : "TR_exact"}}).toJson();
                    }
                    else if (method == "DELETE" && path.endsWith("/" + lease.toUtf8())) { ++deletes; code = 204; }
                    else if (path.endsWith("/screen-sessions") || path.endsWith("/renew")) {
                        require(bytes.mid(headerEnd + 4).contains("\"consent\":true"), "explicit consent in real HTTP request");
                        lastLeaseAccess = access;
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
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) { std::fprintf(stderr, "%s\n", message.toUtf8().constData()); std::fflush(stderr); });
    QGuiApplication app(argc, argv);
    try {
        for (const auto* key : {"DATABASE_URL", "LIVEKIT_API_KEY", "LiVeKiT_ApI_SeCrEt", "LIVEKIT_TOKEN", "GCP_ACCESS_TOKEN", "GCP_MEDIA_SECRET"}) qputenv(key, "dummy-sensitive-value");
        Api api; AccountSession account; ChannelDirectory directory(&account);
        const auto program = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-view-process-fixture.exe"));
        ScreenViewer viewer(&account, &directory, nullptr, {program, 250, 700, 1200, 33});
        qmlRegisterType<ScreenVideoItem>("Gcp.Media", 1, 0, "ScreenVideo");
        QQmlEngine engine; int warnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError>& errors) { warnings += errors.size(); for (const auto& error : errors) std::cerr << error.toString().toStdString() << '\n'; });
        engine.rootContext()->setContextProperty("controller", &viewer); engine.rootContext()->setContextProperty("rooms", &directory); engine.rootContext()->setContextProperty("login", &account);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nScreenViewerPanel { viewer: controller; directory: rooms; account: login; width: 620 }", QUrl::fromLocalFile(QStringLiteral(GCP_SCREEN_QML_DIR) + "/ViewHarness.qml"));
        const std::unique_ptr<QObject> panel(component.create()); if (!panel) std::cerr << component.errorString().toStdString();
        require(panel != nullptr, "actual viewer panel loads");
        auto* videoWindow = panel->findChild<QQuickWindow*>("screenViewerWindow"); require(videoWindow, "actual viewer window exists");
        const auto field = [&](const char* name) { auto* value = panel->findChild<QObject*>(name); require(value, "viewer QML control exists"); return value; };
        const auto click = [&](const char* name) { require(field(name)->property("enabled").toBool(), "viewer QML action enabled"); require(QMetaObject::invokeMethod(field(name), "clicked"), "viewer QML action"); };
        const auto stopped = [&] { require(waitFor([&] { return !viewer.busy(); }, 7000), "bounded viewer cleanup"); require(!viewer.watching() && viewer.image().isNull(), "stop purges decoded pixels"); };
        require(!field("screenRefreshShares")->property("enabled").toBool(), "no discovery while signed out"); viewer.watch(true); require(api.creates == 0, "no signed out viewer lease");
        const auto login = [&] { account.signIn(api.origin(), "test@example.test", "password"); require(waitFor([&] { return account.signedIn() && !directory.busy() && !directory.communities().isEmpty(); }), "login"); directory.chooseCommunity(community); require(waitFor([&] { return !directory.busy() && !directory.channels().isEmpty(); }), "voice directory"); directory.chooseVoiceChannel(channel); };
        login();
        const auto choose = [&] { click("screenRefreshShares"); require(waitFor([&] { return !viewer.busy() && viewer.publishers().size() == 1; }), "authenticated publisher directory"); viewer.choosePublisher(QStringLiteral("00000000-0000-0000-0000-000000000005")); };
        choose(); viewer.watch(false); require(api.creates == 0 && !videoWindow->isVisible(), "discovery and false consent never start viewing");
        click("screenWatch"); require(waitFor([&] { return viewer.watching() && viewer.frameNumber() >= 3 && api.targets >= 2; }), "real pipes, exact track and lease renewal");
        require(viewer.image().size() == QSize(160,88) && viewer.image().pixelColor(20,20) == QColor(30,160,90,255), "actual negotiated RGBA pixels reach controller");
        require(videoWindow->isVisible() && waitFor([&] { return !videoWindow->grabWindow().isNull(); }), "viewer window renders");
        auto rendered = videoWindow->grabWindow(); require(rendered.pixelColor(rendered.width()/2, rendered.height()/2).green() > 120, "SceneGraph renders actual pixels, not status text");
        videoWindow->resize(700, 420); QCoreApplication::processEvents(); rendered = videoWindow->grabWindow();
        require(!rendered.isNull() && rendered.pixelColor(rendered.width()/2,rendered.height()/2).green() > 120, "window resize preserves video");
        click("screenStopWatching"); stopped(); require(api.deletes == 1 && directory.channelId() == channel, "watch Stop leaves voice scope unchanged");
        for (const auto* mode : {"resize", "rotate", "fragmented"}) {
            qputenv("GCP_VIEW_FIXTURE_MODE", mode); click("screenWatch"); require(waitFor([&] { return viewer.watching() && viewer.frameNumber() >= 2; }), "resized/rotated/fragmented frame delivery");
            if (std::string_view(mode) == "resize") require(viewer.image().size() == QSize(320,180), "decoded resize honored");
            if (std::string_view(mode) == "rotate") {
                require(viewer.rotation() == 90, "rotation preserved");
                auto* video = panel->findChild<QQuickItem*>("screenVideo"); require(video, "video item exists");
                // The fixture's red LEFT stripe must appear at the TOP after
                // clockwise rotation, not merely in the controller metadata.
                const auto image = videoWindow->grabWindow();
                const auto displayedHeight = qMin(video->height(), video->width() * 160.0 / 88.0);
                const auto point = video->mapToScene(QPointF(video->width()/2, (video->height()-displayedHeight)/2 + displayedHeight * 0.05));
                const auto color = image.pixelColor(qRound(point.x()*image.width()/videoWindow->width()), qRound(point.y()*image.height()/videoWindow->height()));
                require(color.red() > 180 && color.green() < 80, "SceneGraph actually rotates the stripe pixels");
            }
            viewer.stop(); stopped();
        }
        qunsetenv("GCP_VIEW_FIXTURE_MODE");
        for (const auto* malformed : {"sources", "publish", "subscribe", "identity", "room", "purpose", "url", "fraction", "track"}) { api.malformed = malformed; click("screenWatch"); stopped(); }
        api.malformed.clear(); api.oversized = true; click("screenWatch"); stopped(); api.oversized = false;
        api.hold = true; click("screenWatch"); require(waitFor([&] { return !api.held.isEmpty(); }), "held create"); viewer.stop(); stopped(); api.held.clear(); api.hold = false;
        for (const auto* mode : {"oversized", "bad_rotation", "unsolicited", "oversend", "partial", "failed"}) { qputenv("GCP_VIEW_FIXTURE_MODE", mode); click("screenWatch"); stopped(); }
        qputenv("GCP_VIEW_FIXTURE_MODE", "hang"); click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "hung child starts"); viewer.stop(); stopped();
        qputenv("GCP_VIEW_FIXTURE_MODE", "static"); click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "static image");
        videoWindow->hide(); stopped(); const auto createsBefore = api.creates; videoWindow->show(); QCoreApplication::processEvents(); require(!viewer.watching() && api.creates == createsBefore, "show never automatically resumes"); videoWindow->hide();
        qunsetenv("GCP_VIEW_FIXTURE_MODE"); click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "renewal denial setup"); api.deny = 403; stopped(); api.deny = 0;
        api.ttl = 1; click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "short lease"); api.hold = true; stopped(); api.hold = false; api.ttl = 60; api.held.clear();
        click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "scope setup"); directory.chooseVoiceChannel(QStringLiteral("00000000-0000-0000-0000-000000000004")); stopped();
        directory.chooseVoiceChannel(channel); choose(); click("screenWatch"); require(waitFor([&] { return viewer.watching(); }), "logout setup"); account.signOut(); stopped();
        require(warnings == 0, "viewer QML has no warnings");
        std::cout << "Viewer QML/SceneGraph rendered RGBA, resize/rotation, exact-track HTTP/pipes, consent, frame limits, revocation, expiry, visibility and bounded cleanup passed. Protocol peer only; no SFU or physical screen.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
