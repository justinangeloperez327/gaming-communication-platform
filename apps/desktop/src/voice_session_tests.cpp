#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "VoiceSession.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonArray>
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
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &condition, int timeout = 4000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}
const QString community = QStringLiteral("00000000-0000-0000-0000-000000000001");
const QString channel = QStringLiteral("00000000-0000-0000-0000-000000000002");

// Auth and native process fixtures exercise actual HTTP, QProcess stdin/stdout,
// cancellation and timers. They are not SFU/audio/hardware acceptance tests.
struct ApiFixture {
    QTcpServer server;
    bool revoke = false;
    bool hold = false;
    bool holdChannels = false;
    bool channelExists = true;
    int channelLists = 0;
    int closedHeldChannels = 0;
    int joins = 0;
    int cancelledTokens = 0;
    QList<QPointer<QTcpSocket>> heldTokens;
    int loginTtl = 900, refreshes = 0, voiceDenials = 0;
    bool holdRefresh = false, rejectFreshVoice = false;
    QPointer<QTcpSocket> refreshReply;
    QString accessToken = QString(64, 'a'), refreshToken = QString(64, 'b'), lastVoiceAccess;
    QString voiceUrl = QStringLiteral("wss://fixture.invalid");
    QByteArray sessionBody(int ttl) const {
        return QJsonDocument(QJsonObject{
            {"access_token", accessToken}, {"refresh_token", refreshToken},
            {"expires_in", ttl}, {"user", QJsonObject{{"display_name", "Test"}}}
        }).toJson(QJsonDocument::Compact);
    }
    QByteArray tokenBody() const {
        return QJsonDocument(QJsonObject{
            {"server_url", voiceUrl}, {"participant_token", QString(64, 'x')}, {"expires_in", 300}
        }).toJson(QJsonDocument::Compact);
    }
    static void respond(QTcpSocket *socket, int status, const QByteArray &body) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n"
                      "Content-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    void completeTokens() {
        for (const auto &socket : heldTokens)
            if (socket && socket->state() == QAbstractSocket::ConnectedState) respond(socket, 201, tokenBody());
        heldTokens.clear();
    }
    void completeRefresh() {
        holdRefresh = false;
        if (refreshReply && refreshReply->state() == QAbstractSocket::ConnectedState)
            respond(refreshReply, 200, sessionBody(900));
        refreshReply = nullptr;
    }
    ApiFixture() {
        require(server.listen(QHostAddress::LocalHost), "HTTP fixture listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                if (socket->property("answered").toBool()) { socket->readAll(); return; }
                auto request = socket->property("request").toByteArray();
                request += socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                socket->setProperty("answered", true);
                const auto path = request.split(' ').value(1);
                QByteArray body;
                int status = 200;
                if (path == "/v1/auth/login") {
                    body = sessionBody(loginTtl);
                } else if (path == "/v1/auth/refresh") {
                    ++refreshes;
                    accessToken = QString(64, 'c');
                    refreshToken = QString(64, 'd');
                    if (holdRefresh) { refreshReply = socket; return; }
                    body = sessionBody(900);
                } else if (path == "/v1/auth/logout") {
                    status = 204;
                } else if (!request.toLower().contains("authorization: bearer " + accessToken.toUtf8())) {
                    status = 401;
                    if (path.endsWith("/livekit-token")) ++voiceDenials;
                } else if (path == "/v1/communities") {
                    body = "[{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}]";
                } else if (path == "/v1/communities/" + community.toUtf8()) {
                    body = "{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\",\"role\":\"member\"}";
                } else if (path.endsWith("/channels")) {
                    ++channelLists;
                    if (holdChannels) {
                        QObject::connect(socket, &QTcpSocket::disconnected, &server,
                                         [this]() { ++closedHeldChannels; });
                        return;
                    }
                    body = "[{\"id\":\"" + channel.toUtf8() + "\",\"name\":\"team-voice\",\"channel_kind\":\"voice\",\"position\":0}]";
                    if (channelExists) {
                        auto channels = QJsonDocument::fromJson(body).array();
                        channels.append(QJsonObject{{"id", "00000000-0000-0000-0000-000000000003"},
                            {"name", "team-text"}, {"channel_kind", "text"}, {"position", 1}});
                        body = QJsonDocument(channels).toJson(QJsonDocument::Compact);
                    } else body = "[]";
                } else if (path.endsWith("/livekit-token")) {
                    ++joins;
                    lastVoiceAccess = accessToken;
                    if (hold) {
                        heldTokens.append(socket);
                        QObject::connect(socket, &QTcpSocket::disconnected, &server, [this]() { ++cancelledTokens; });
                        return;
                    }
                    status = rejectFreshVoice ? 401 : revoke ? 403 : 201;
                    if (status == 401) ++voiceDenials;
                    body = tokenBody();
                } else status = 404;
                respond(socket, status, body);
            });
        });
    }
    ~ApiFixture() {
        server.close();
        for (auto *socket : server.findChildren<QTcpSocket *>()) { socket->disconnect(); socket->abort(); }
    }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        // The actual child fixture refuses to start if these dummy credentials
        // survive inheritance. No test prints environment values or secrets.
        qputenv("DATABASE_URL", "dummy-database-credential");
        qputenv("LIVEKIT_API_KEY", "dummy-admin-key");
        qputenv("LiVeKiT_ApI_SeCrEt", "dummy-admin-secret");
        qputenv("GCP_MEDIA_SECRET", "dummy-media-secret");
        qputenv("GCP_POSTGRES_ADMIN_PASSWORD", "dummy-db-admin");
        qputenv("GCP_ACCESS_TOKEN", "dummy-access");
        qputenv("GCP_REFRESH_TOKEN", "dummy-refresh");
        qputenv("LIVEKIT_URL", "wss://inherited.invalid");
        qputenv("LIVEKIT_TOKEN", "dummy-inherited-room-token");
        qputenv("GCP_FIXTURE_VALIDATE_ENV", "1");
        qputenv("GCP_FIXTURE_RUNTIME_VALUE", "preserved");
        ApiFixture api;
        AccountSession account;
        ChannelDirectory directory{&account};
        const auto executable = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gcp-voice-process-fixture.exe"));
        VoiceSession voice{&account, &directory, nullptr, VoiceRuntime{executable, 500}};
        QQmlEngine engine;
        int qmlWarnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &) { ++qmlWarnings; });
        engine.rootContext()->setContextProperty("voiceController", &voice);
        engine.rootContext()->setContextProperty("accountController", &account);
        engine.rootContext()->setContextProperty("directoryController", &directory);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \".\"\nVoicePanel { voice: voiceController; account: accountController; directory: directoryController; width: 620 }",
                          QUrl::fromLocalFile(QStringLiteral(GCP_VOICE_QML_DIR) + "/VoiceHarness.qml"));
        QQuickWindow window;
        window.resize(660, 700);
        const std::unique_ptr<QObject> panel(component.create());
        if (!panel) std::cerr << component.errorString().toStdString();
        require(panel != nullptr, "actual voice panel QML loads");
        qobject_cast<QQuickItem *>(panel.get())->setParentItem(window.contentItem());
        window.show();
        const auto field = [&](const char *name) {
            auto *object = panel->findChild<QObject *>(QString::fromLatin1(name));
            require(object != nullptr, "voice QML control exists"); return object;
        };
        const auto click = [&](const char *name) {
            require(field(name)->property("enabled").toBool(), "voice QML action enabled");
            require(QMetaObject::invokeMethod(field(name), "clicked"), "voice QML button action");
        };
        require(!field("voiceMicrophoneConsent")->property("enabled").toBool(), "consent disabled before sign-in and room selection");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "normal scan completion");
        require(voice.inputs().size() == 1 && voice.outputs().size() == 1, "device scan parsing");
        voice.selectInput(QStringLiteral("fixture-input"));
        voice.selectOutput(QStringLiteral("fixture-output"));
        voice.selectInput(QStringLiteral("unknown-input"));
        voice.selectOutput(QStringLiteral("unknown-output"));
        require(voice.inputId() == QStringLiteral("fixture-input") &&
                voice.outputId() == QStringLiteral("fixture-output"), "unknown device IDs rejected");
        const auto previousInputs = voice.inputs();
        const auto previousOutputs = voice.outputs();
        qputenv("GCP_FIXTURE_DEVICE_SCAN", "oversized");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "oversized scan stops before watchdog");
        qunsetenv("GCP_FIXTURE_DEVICE_SCAN");
        require(voice.status().contains("scan failed"), "oversized scan fails closed");
        require(voice.inputs() == previousInputs && voice.outputs() == previousOutputs,
                "failed scan preserves previous device list");
        require(voice.inputId() == QStringLiteral("fixture-input") &&
                voice.outputId() == QStringLiteral("fixture-output"), "failed scan preserves selection");
        qputenv("GCP_FIXTURE_DEVICE_SCAN", "stderr");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "stderr flood scan completion");
        qunsetenv("GCP_FIXTURE_DEVICE_SCAN");
        require(voice.inputs().size() == 1 && voice.outputs() == previousOutputs &&
                voice.inputs().first().toMap().value("name").toString() == "Recovered microphone",
                "scan retries after overflow and discards stderr");
        require(voice.inputId() == QStringLiteral("fixture-input") &&
                voice.outputId() == QStringLiteral("fixture-output"), "successful rescan preserves device identity");
        qputenv("GCP_FIXTURE_DEVICE_SCAN", "empty");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "removed device scan completion");
        qunsetenv("GCP_FIXTURE_DEVICE_SCAN");
        require(voice.inputId().isEmpty() && voice.outputId().isEmpty() &&
                voice.inputs().isEmpty() && voice.outputs().isEmpty(), "removed devices revert to default");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "devices return after removal");
        require(voice.inputId().isEmpty() && voice.outputId().isEmpty(), "returned devices do not silently reselect");
        voice.selectInput(QStringLiteral("fixture-input"));
        voice.selectOutput(QStringLiteral("fixture-output"));
        qputenv("GCP_FIXTURE_EXPECT_DEVICE_ARGS", "1");
        account.signIn(api.origin(), QStringLiteral("test@example.test"), QStringLiteral("test password"));
        require(waitFor([&]() { return !directory.communities().isEmpty(); }), "community listing");
        require(directory.communityId().isEmpty() && directory.channelId().isEmpty(),
                "listing communities does not select one");
        directory.chooseCommunity(community);
        require(waitFor([&]() { return !directory.channels().isEmpty(); }), "channel listing");
        require(directory.channelId().isEmpty(), "listing voice channels does not select one");
        directory.chooseVoiceChannel(channel);
        field("voiceMicrophoneConsent")->setProperty("checked", true);
        require(panel->property("microphoneConsent").toBool(), "consent control reaches the panel");
        click("voiceListenOnly");
        require(waitFor([&]() { return voice.connected(); }), "QML listen-only join");
        require(!panel->property("microphoneConsent").toBool(), "listen-only consumes pending microphone consent");
        const auto joinsBeforeText = api.joins;
        directory.chooseTextChannel(QStringLiteral("00000000-0000-0000-0000-000000000003"));
        require(directory.textChannelId().endsWith("0003") && directory.channelId() == channel &&
                voice.connected() && api.joins == joinsBeforeText, "text selection preserves the active voice process and room");
        click("voiceLeave");
        require(waitFor([&]() { return !voice.busy(); }), "QML leave after listen-only");
        require(!field("voiceJoinMicrophone")->property("enabled").toBool(), "stopped listen-only call cannot reuse old consent");
        const auto beforeConsent = api.joins;
        voice.joinWithMicrophone(false);
        require(!voice.busy() && api.joins == beforeConsent, "consent required before authorization");
        field("voiceMicrophoneConsent")->setProperty("checked", true);
        click("voiceJoinMicrophone");
        require(waitFor([&]() { return voice.connected(); }), "controlled subprocess join");
        require(voice.muted() && !voice.transmitting(), "call must start muted");
        click("voiceMute");
        require(waitFor([&]() { return voice.transmitting(); }), "native unmute acknowledgment");
        click("voiceMute");
        require(waitFor([&]() { return !voice.transmitting(); }), "native mute acknowledgment");
        voice.setPushToTalk(true);
        require(voice.pushToTalk() && voice.muted(), "mode switch must mute");
        api.revoke = true;
        require(waitFor([&]() { return !voice.running() && !voice.busy(); }), "revoked access must stop voice");
        require(api.joins >= 2 && voice.status().contains("recheck failed"), "periodic backend authorization");

        api.revoke = false;
        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "delay-ready");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.running(); }), "native process starts before audio readiness");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        require(!voice.connected() && !field("voiceMute")->property("enabled").toBool(), "mic controls wait for native audio readiness");
        voice.setMuted(false);
        require(voice.muted() && !voice.transmitting(), "unmute ignored before audio is ready");
        require(waitFor([&]() { return voice.connected(); }), "audio readiness acknowledgement enables connected state");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "leave after delayed audio setup");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "reconnect");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "join before SDK reconnect fixture");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        api.hold = true;
        const auto beforeRecovery = api.joins;
        voice.setMuted(false);
        require(waitFor([&]() { return api.joins > beforeRecovery && !voice.connected(); }), "SDK reconnect immediately rechecks backend access");
        require(voice.muted() && !voice.transmitting() && voice.status().contains("checking current"), "recovery stays muted while authorization is pending");
        voice.setMuted(false);
        require(voice.muted() && !voice.transmitting(), "unmute rejected before recovery authorization");
        api.hold = false;
        api.completeTokens();
        require(waitFor([&]() { return voice.connected(); }), "authorized SDK recovery");
        require(voice.muted() && !voice.transmitting(), "recovery never silently re-arms microphone");
        voice.setMuted(false);
        require(waitFor([&]() { return voice.transmitting(); }), "fresh Enable mic after recovery");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "leave recovered call");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "reconnect");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "join before denied recovery");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        api.revoke = true;
        voice.setMuted(false);
        require(waitFor([&]() { return !voice.busy(); }), "revoked access on reconnect stops child");
        require(voice.status().contains("recheck failed") && !voice.transmitting(), "denied recovery remains stopped");
        api.revoke = false;

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "mute-timeout");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "join before missing mute acknowledgement");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        voice.setMuted(false);
        require(waitFor([&]() { return voice.transmitting(); }), "unmute before missing acknowledgement");
        voice.setMuted(true);
        require(waitFor([&]() { return !voice.busy(); }), "missing SDK mute acknowledgement stops child");
        require(voice.status().contains("not acknowledged") && !voice.transmitting(), "mute timeout is actionable and safe");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "lost");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "join before definitive disconnect");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        voice.setMuted(false);
        require(waitFor([&]() { return !voice.busy(); }), "terminal SDK disconnect stops voice");
        require(voice.status().contains("connection lost") && !voice.transmitting(), "disconnect reason survives child exit");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "microphone-failed");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return !voice.busy(); }), "microphone failure stops child");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        require(voice.status().contains("Windows microphone permissions") && !voice.transmitting(), "microphone error gives actionable permission/device guidance");

        for (const auto *url : {"wss:///", "wss://fixture.invalid/path", "wss://fixture.invalid?token=x", "wss://fixture.invalid/#fragment"}) {
            api.voiceUrl = QString::fromLatin1(url);
            voice.joinListenOnly();
            require(waitFor([&]() { return !voice.busy(); }), "invalid room URL response completes");
            require(!voice.running() && voice.status().contains("unsafe"), "unsafe room URL never launches child");
        }
        api.voiceUrl = QStringLiteral("wss://fixture.invalid");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "burst");
        voice.joinListenOnly();
        require(waitFor([&]() { return voice.connected(); }), "listen-only join after diagnostic burst");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        voice.setMuted(false);
        require(!voice.microphoneEnabled() && !voice.transmitting(), "listen-only cannot publish mic");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "orderly Leave");

        qputenv("GCP_FIXTURE_VOICE_OUTPUT", "oversized");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "join before status overflow");
        qunsetenv("GCP_FIXTURE_VOICE_OUTPUT");
        voice.setMuted(false);
        require(waitFor([&]() { return !voice.busy(); }), "status overflow must stop voice");
        require(voice.status().contains("exceeded limit") && !voice.transmitting(),
                "status overflow leaves microphone state safe");
        voice.joinListenOnly();
        require(waitFor([&]() { return voice.connected(); }), "rejoin after status overflow");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "Leave after overflow retry");

        api.hold = true;
        const auto previous = api.joins;
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return api.joins > previous; }), "held join request");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "cancel pending join");
        require(!voice.running(), "cancelled join cannot launch microphone");
        api.hold = false;
        api.completeTokens();
        require(!voice.running(), "late authorization after cancellation cannot reopen microphone");

        api.hold = true;
        const auto beforeDestroy = api.joins;
        auto transient = std::make_unique<VoiceSession>(&account, &directory, nullptr, VoiceRuntime{executable, 500});
        transient->joinWithMicrophone(true);
        require(waitFor([&]() { return api.joins > beforeDestroy; }), "pending authorization before controller destruction");
        const auto beforeAbort = api.cancelledTokens;
        transient.reset();
        require(waitFor([&]() { return api.cancelledTokens > beforeAbort; }), "destruction aborts pending authorization at the peer");
        api.hold = false;
        api.completeTokens();

        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "rejoin after cancellation");
        account.signOut();
        require(!panel->property("microphoneConsent").toBool(), "logout clears microphone consent");
        require(waitFor([&]() { return !voice.busy(); }), "logout stops voice");
        account.signIn(api.origin(), QStringLiteral("test@example.test"), QStringLiteral("test password"));
        require(waitFor([&]() { return !directory.communities().isEmpty(); }), "directory reload after login");
        api.holdChannels = true;
        const auto previousChannelLists = api.channelLists;
        directory.chooseCommunity(community);
        require(waitFor([&]() { return api.channelLists > previousChannelLists; }), "held channel list request");
        require(directory.busy(), "held channel list stays pending");
        account.signOut();
        require(waitFor([&]() { return api.closedHeldChannels == 1 && !directory.busy(); }),
                "logout aborts pending channel request at peer");
        require(directory.communities().isEmpty() && directory.channels().isEmpty() &&
                directory.communityId().isEmpty() && directory.channelId().isEmpty(),
                "logout clears directory and selection");
        api.holdChannels = false;
        account.signIn(api.origin(), QStringLiteral("test@example.test"), QStringLiteral("test password"));
        require(waitFor([&]() { return !directory.communities().isEmpty(); }), "retry after cancelled directory request");
        directory.chooseCommunity(community);
        require(waitFor([&]() { return !directory.channels().isEmpty(); }), "channel reload after cancellation");
        directory.chooseVoiceChannel(channel);
        voice.joinListenOnly();
        require(waitFor([&]() { return voice.connected(); }), "voice connected before directory refresh");
        directory.refresh();
        require(directory.communityId() == community && directory.channelId() == channel,
                "refresh retains the selected room while revalidating");
        require(waitFor([&]() { return !directory.busy(); }), "directory refresh completion");
        require(directory.communityId() == community && directory.channelId() == channel && voice.connected(),
                "harmless refresh preserves the active voice process");
        api.channelExists = false;
        directory.refresh();
        require(waitFor([&]() { return !directory.busy() && !voice.busy(); }), "removed room stops voice");
        require(directory.channelId().isEmpty() && directory.channels().isEmpty() && directory.communityId() == community,
                "removed channel clears only the voice selection");
        account.signOut();
        qunsetenv("GCP_FIXTURE_EXPECT_DEVICE_ARGS");
        const auto exerciseRotation = [&](bool denyFresh, bool cancel) {
            ApiFixture rotatingApi;
            rotatingApi.loginTtl = 61; // actual AccountSession refresh timer fires in one second
            rotatingApi.holdRefresh = true;
            AccountSession rotatingAccount;
            ChannelDirectory rotatingDirectory(&rotatingAccount);
            VoiceSession rotatingVoice(&rotatingAccount, &rotatingDirectory, nullptr, VoiceRuntime{executable, 100});
            int childStarts = 0;
            QObject::connect(&rotatingVoice, &VoiceSession::statusChanged, &rotatingVoice, [&]() {
                if (rotatingVoice.status().startsWith("Native voice process started")) ++childStarts;
            });
            rotatingAccount.signIn(rotatingApi.origin(), "rotation@example.test", "private-test-password");
            require(waitFor([&]() { return !rotatingDirectory.communities().isEmpty(); }), "rotation fixture sign-in");
            rotatingDirectory.chooseCommunity(community);
            require(waitFor([&]() { return !rotatingDirectory.channels().isEmpty(); }), "rotation fixture channels");
            rotatingDirectory.chooseVoiceChannel(channel);
            rotatingVoice.joinWithMicrophone(true);
            require(waitFor([&]() { return rotatingVoice.connected(); }), "call before actual account refresh timer");
            require(waitFor([&]() { return rotatingApi.refreshes == 1 && rotatingApi.voiceDenials == 1; }), "voice check overlaps a committed rotation before refresh response");
            require(rotatingAccount.busy() && rotatingVoice.running() && rotatingVoice.busy(), "rotation 401 waits without restarting the child");
            rotatingApi.rejectFreshVoice = denyFresh;
            const auto beforeFresh = rotatingApi.joins;
            if (cancel) rotatingVoice.leave();
            rotatingApi.completeRefresh();
            require(waitFor([&]() { return !rotatingAccount.busy(); }), "rotated credentials reach account controller");
            if (cancel) {
                require(waitFor([&]() { return !rotatingVoice.busy(); }), "Leave cancels deferred rotation retry");
                require(!rotatingVoice.running() && rotatingApi.joins == beforeFresh, "refresh completion cannot reopen a cancelled call");
            } else if (denyFresh) {
                require(waitFor([&]() { return !rotatingVoice.busy(); }), "fresh-token denial stops call");
                require(rotatingApi.voiceDenials == 2 && rotatingVoice.status().contains("recheck failed"), "rotation retry is bounded to one and cannot bypass denial");
            } else {
                require(waitFor([&]() { return rotatingApi.joins >= beforeFresh + 2 && rotatingApi.lastVoiceAccess == QString(64, 'c'); }), "rotated authorization succeeds and periodic checks resume");
                require(rotatingVoice.connected() && rotatingVoice.running() && rotatingVoice.muted() && childStarts == 1,
                        "rotation preserves call and never silently re-arms microphone");
            }
            rotatingVoice.leave();
            require(waitFor([&]() { return !rotatingVoice.busy(); }), "rotation fixture cleanup");
            rotatingAccount.signOut();
        };
        exerciseRotation(false, false);
        exerciseRotation(true, false);
        exerciseRotation(false, true);
        require(qgetenv("DATABASE_URL") == "dummy-database-credential" &&
                qgetenv("LIVEKIT_TOKEN") == "dummy-inherited-room-token",
                "child filtering must not mutate the parent environment");
        require(qmlWarnings == 0, "voice panel produced QML warnings");
        std::cout << "Offline Qt voice QML, subprocess, recovery and authorization tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
