#include "AccountSession.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()> &condition) {
    if (condition()) return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (condition()) loop.quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return condition();
}
struct Fixture {
    QTcpServer server;
    int registrationStatus = 201;
    int loginStatus = 200;
    bool owner = false;
    bool holdRegistration = false;
    bool holdInvitations = false;
    bool heldDisconnected = false;
    bool oversizedList = false;
    int registrations = 0;
    int invitations = 0;
    int invitationLists = 0;
    QJsonObject registration;
    QJsonArray pending;
    const QString id = QStringLiteral("00000000-0000-0000-0000-000000000001");
    static void reply(QTcpSocket *socket, int status, const QByteArray &body = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QByteArray session() const {
        return QJsonDocument(QJsonObject{{"access_token", QString(64, 'a')}, {"refresh_token", QString(64, 'b')},
            {"expires_in", 900}, {"server_owner", owner}, {"user", QJsonObject{{"display_name", "Player"}}}}).toJson(QJsonDocument::Compact);
    }
    Fixture() {
        require(server.listen(QHostAddress::LocalHost), "fixture listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                auto bytes = socket->property("request").toByteArray();
                bytes += socket->readAll();
                socket->setProperty("request", bytes);
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("answered").toBool()) return;
                int length = 0;
                bool authorized = false;
                for (const auto &line : bytes.left(end).split('\n')) {
                    const auto colon = line.indexOf(':');
                    if (colon < 0) continue;
                    const auto name = line.left(colon).trimmed().toLower();
                    const auto value = line.mid(colon + 1).trimmed();
                    if (name == "content-length") length = value.toInt();
                    if (name == "authorization") authorized = value == "Bearer " + QByteArray(64, 'a');
                }
                if (bytes.size() < end + 4 + length) return;
                socket->setProperty("answered", true);
                const auto path = bytes.split(' ').value(1);
                const auto object = QJsonDocument::fromJson(bytes.mid(end + 4, length)).object();
                if (path == "/v1/auth/register") {
                    ++registrations; registration = object;
                    if (holdRegistration) {
                        QObject::connect(socket, &QTcpSocket::disconnected, &server, [this]() { heldDisconnected = true; });
                        return;
                    }
                    reply(socket, registrationStatus, registrationStatus == 201 ? session() : QByteArray("{\"error\":\"private-server-detail\"}"));
                } else if (path == "/v1/auth/login") {
                    reply(socket, loginStatus, loginStatus == 200 ? session() : QByteArray("{\"error\":\"private-server-detail\"}"));
                } else if (path == "/v1/auth/logout") reply(socket, 204);
                else if (!authorized) reply(socket, 401, "{}");
                else if (path == "/v1/account-invitations" && bytes.startsWith("GET ")) {
                    ++invitationLists;
                    reply(socket, 200, oversizedList ? QByteArray(100000, 'x') : QJsonDocument(pending).toJson(QJsonDocument::Compact));
                } else if (path == "/v1/account-invitations") {
                    ++invitations;
                    if (holdInvitations) {
                        QObject::connect(socket, &QTcpSocket::disconnected, &server, [this]() { heldDisconnected = true; });
                        return;
                    }
                    pending = QJsonArray{QJsonObject{{"id", id}, {"email", object.value("email")}, {"expires_in", 86400}}};
                    auto result = pending.first().toObject(); result.insert("token", QString(64, 'c'));
                    reply(socket, 201, QJsonDocument(result).toJson(QJsonDocument::Compact));
                } else if (path == "/v1/account-invitations/" + id.toUtf8() && bytes.startsWith("DELETE ")) {
                    pending = {}; reply(socket, 204);
                } else reply(socket, 404, "{}");
            });
        });
    }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        Fixture fixture;
        AccountSession account;
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(GCP_ACCOUNT_PANEL_PATH)));
        const std::unique_ptr<QObject> panel(component.createWithInitialProperties({{"account", QVariant::fromValue(&account)}}));
        if (!panel) std::cerr << component.errorString().toStdString();
        require(panel != nullptr, "account QML creation");
        panel->setProperty("width", 560);
        const auto field = [&](const char *name) {
            auto *item = panel->findChild<QObject *>(QString::fromLatin1(name));
            require(item != nullptr, "account form field exists"); return item;
        };
        const auto fill = [&]() {
            panel->setProperty("registering", true);
            field("accountOrigin")->setProperty("text", fixture.origin());
            field("accountEmail")->setProperty("text", "friend@example.test");
            field("accountHandle")->setProperty("text", "friend");
            field("accountDisplayName")->setProperty("text", "Friend");
            field("accountPassword")->setProperty("text", "a-long-private-test-password");
            field("accountConfirmation")->setProperty("text", "a-long-private-test-password");
            field("accountInvitation")->setProperty("text", QString(64, 'c'));
        };
        fill();
        field("accountConfirmation")->setProperty("text", "mismatched");
        require(QMetaObject::invokeMethod(panel.get(), "submit"), "QML submission");
        require(fixture.registrations == 0 && !account.busy(), "mismatched confirmation prevents request");
        fill();
        require(QMetaObject::invokeMethod(panel.get(), "submit"), "QML registration");
        require(field("accountPassword")->property("text").toString().isEmpty() &&
                field("accountInvitation")->property("text").toString().isEmpty(), "dispatched secrets are cleared");
        require(waitFor([&]() { return account.signedIn() && !account.busy(); }), "HTTP 201 signs in");
        require(fixture.registrations == 1 && fixture.registration.value("invitation_token").toString() == QString(64, 'c'), "actual registration JSON");
        require(!account.serverOwner(), "registration does not grant owner UI");
        account.inviteFriend(QStringLiteral("x@example.test"));
        require(fixture.invitations == 0, "member cannot invoke owner controls");
        account.signOut();
        fixture.registrationStatus = 403;
        fill(); QMetaObject::invokeMethod(panel.get(), "submit");
        require(waitFor([&]() { return !account.busy(); }) && !account.signedIn(), "rejected invitation stays signed out");
        require(account.status().contains("Invitation") && !account.status().contains("private-server-detail"), "actionable invitation error without server text");
        fixture.registrationStatus = 409;
        fill(); QMetaObject::invokeMethod(panel.get(), "submit");
        require(waitFor([&]() { return !account.busy(); }) && account.status().contains("already in use"), "conflict error");
        for (const int status : {401, 503}) {
            fixture.loginStatus = status;
            account.signIn(fixture.origin(), QStringLiteral("owner@example.test"), QStringLiteral("private-password"));
            require(waitFor([&]() { return !account.busy(); }) && !account.signedIn(), "login rejection");
            require(account.status().contains(status == 401 ? "incorrect" : "not ready"), "specific login errors");
        }
        fixture.loginStatus = 200; fixture.owner = true;
        account.signIn(fixture.origin(), QStringLiteral("owner@example.test"), QStringLiteral("private-password"));
        require(waitFor([&]() { return account.signedIn() && !account.busy() && !account.invitationBusy() && fixture.invitationLists > 0; }), "owner login and initial invitation list");
        require(account.serverOwner(), "server owner role");
        account.inviteFriend(QStringLiteral("new@example.test"));
        const bool created = waitFor([&]() { return !account.invitationBusy() && account.invitationCode().size() == 64 && account.invitations().size() == 1; });
        if (!created) std::cerr << account.invitationStatus().toStdString() << '\n';
        require(created, "create and list invitations");
        require(account.invitationEmail() == QStringLiteral("new@example.test"), "code email binding displayed");
        account.revokeInvitation();
        require(waitFor([&]() { return !account.invitationBusy() && account.invitationCode().isEmpty() && account.invitations().isEmpty(); }), "revoke and refresh");
        fixture.pending = QJsonArray{QJsonObject{{"id", fixture.id}, {"email", "earlier@example.test"}, {"expires_in", 3600}}};
        account.loadInvitations();
        require(waitFor([&]() { return !account.invitationBusy() && account.invitations().size() == 1; }), "load invitation from earlier session");
        account.revokePendingInvitation(fixture.id);
        require(waitFor([&]() { return !account.invitationBusy() && account.invitations().isEmpty(); }), "revoke earlier invitation without its raw code");
        fixture.oversizedList = true; account.loadInvitations();
        require(waitFor([&]() { return !account.invitationBusy(); }) && account.signedIn() && account.invitations().isEmpty(), "oversized management reply cannot corrupt session");
        fixture.oversizedList = false;
        fixture.holdInvitations = true;
        const int before = fixture.invitations;
        account.inviteFriend(QStringLiteral("later@example.test"));
        require(waitFor([&]() { return fixture.invitations > before; }), "held invitation request");
        account.signOut();
        require(waitFor([&]() { return fixture.heldDisconnected; }), "logout aborts invitation transport");
        require(!account.serverOwner() && account.invitationCode().isEmpty() && account.invitations().isEmpty(), "logout clears privileged data");
        fixture.holdRegistration = true; fixture.heldDisconnected = false;
        {
            auto temporary = std::make_unique<AccountSession>();
            temporary->createAccount(fixture.origin(), "temporary", "Temporary", "temp@example.test", "private-password-123", QString(64, 'c'));
            const int beforeRegistration = fixture.registrations;
            require(waitFor([&]() { return fixture.registrations > beforeRegistration; }), "held account request");
        }
        require(waitFor([&]() { return fixture.heldDisconnected; }), "destruction aborts pending account request");
        std::cout << "Qt account onboarding, invitation controls and cancellation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
