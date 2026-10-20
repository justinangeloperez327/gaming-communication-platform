#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "VoiceSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
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
    int joins = 0;
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
                    body = QJsonDocument(QJsonObject{
                        {"access_token", QString(64, 'a')}, {"refresh_token", QString(64, 'b')},
                        {"expires_in", 900}, {"user", QJsonObject{{"display_name", "Test"}}}
                    }).toJson(QJsonDocument::Compact);
                } else if (path == "/v1/auth/logout") {
                    status = 204;
                } else if (!request.toLower().contains("authorization: bearer " + QByteArray(64, 'a'))) {
                    status = 401;
                } else if (path == "/v1/communities") {
                    body = "[{\"id\":\"" + community.toUtf8() + "\",\"name\":\"Test\"}]";
                } else if (path.endsWith("/channels")) {
                    body = "[{\"id\":\"" + channel.toUtf8() + "\",\"name\":\"Voice\",\"channel_kind\":\"voice\"}]";
                } else if (path.endsWith("/livekit-token")) {
                    ++joins;
                    if (hold) return; // Let the desktop cancel a real pending reply.
                    status = revoke ? 403 : 201;
                    body = QJsonDocument(QJsonObject{
                        {"server_url", "wss://fixture.invalid"},
                        {"participant_token", QString(64, 'x')}, {"expires_in", 300}
                    }).toJson(QJsonDocument::Compact);
                } else status = 404;
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n"
                              "Content-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        ApiFixture api;
        AccountSession account;
        ChannelDirectory directory{&account};
        const auto executable = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gcp-voice-process-fixture.exe"));
        VoiceSession voice{&account, &directory, nullptr, VoiceRuntime{executable, 500}};
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "normal scan completion");
        require(voice.inputs().size() == 1 && voice.outputs().size() == 1, "device scan parsing");
        const auto previousInputs = voice.inputs();
        const auto previousOutputs = voice.outputs();
        qputenv("GCP_FIXTURE_DEVICE_SCAN", "oversized");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "oversized scan stops before watchdog");
        qunsetenv("GCP_FIXTURE_DEVICE_SCAN");
        require(voice.status().contains("scan failed"), "oversized scan fails closed");
        require(voice.inputs() == previousInputs && voice.outputs() == previousOutputs,
                "failed scan preserves previous device list");
        qputenv("GCP_FIXTURE_DEVICE_SCAN", "stderr");
        voice.scanDevices();
        require(waitFor([&]() { return !voice.scanning(); }), "stderr flood scan completion");
        qunsetenv("GCP_FIXTURE_DEVICE_SCAN");
        require(voice.inputs().size() == 1 && voice.outputs() == previousOutputs &&
                voice.inputs().first().toMap().value("name").toString() == "Recovered microphone",
                "scan retries after overflow and discards stderr");
        account.signIn(api.origin(), QStringLiteral("test@example.test"), QStringLiteral("test password"));
        require(waitFor([&]() { return !directory.communities().isEmpty(); }), "community listing");
        directory.chooseCommunity(community);
        require(waitFor([&]() { return !directory.channels().isEmpty(); }), "channel listing");
        directory.chooseVoiceChannel(channel);
        voice.joinWithMicrophone(false);
        require(!voice.busy() && api.joins == 0, "consent required before authorization");
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "controlled subprocess join");
        require(voice.muted() && !voice.transmitting(), "call must start muted");
        voice.setMuted(false);
        require(waitFor([&]() { return voice.transmitting(); }), "native unmute acknowledgment");
        voice.setMuted(true);
        require(waitFor([&]() { return !voice.transmitting(); }), "native mute acknowledgment");
        voice.setPushToTalk(true);
        require(voice.pushToTalk() && voice.muted(), "mode switch must mute");
        api.revoke = true;
        require(waitFor([&]() { return !voice.running() && !voice.busy(); }), "revoked access must stop voice");
        require(api.joins >= 2 && voice.status().contains("recheck failed"), "periodic backend authorization");

        api.revoke = false;
        voice.joinListenOnly();
        require(waitFor([&]() { return voice.connected(); }), "listen-only join");
        voice.setMuted(false);
        require(!voice.microphoneEnabled() && !voice.transmitting(), "listen-only cannot publish mic");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "orderly Leave");

        api.hold = true;
        const auto previous = api.joins;
        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return api.joins > previous; }), "held join request");
        voice.leave();
        require(waitFor([&]() { return !voice.busy(); }), "cancel pending join");
        require(!voice.running(), "cancelled join cannot launch microphone");
        api.hold = false;

        voice.joinWithMicrophone(true);
        require(waitFor([&]() { return voice.connected(); }), "rejoin after cancellation");
        account.signOut();
        require(waitFor([&]() { return !voice.busy(); }), "logout stops voice");
        std::cout << "Offline Qt voice subprocess and authorization tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
