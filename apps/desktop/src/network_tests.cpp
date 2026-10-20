#include "BoundedReply.h"
#include "AccountSession.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <memory>

namespace {
// Local HTTP fixture; exercises Qt's real incremental network buffering.
bool boundedResponse(bool advertised) {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, advertised]() {
            socket->readAll();
            QByteArray body(32768, 'x');
            socket->write(advertised
                ? "HTTP/1.1 200 OK\r\nContent-Length: 32768\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
            socket->write(body);
            socket->disconnectFromHost();
        });
    });
    QNetworkAccessManager network;
    auto *reply = network.get(QNetworkRequest(QUrl(
        QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()))));
    boundReply(reply, 1024);
    QEventLoop loop;
    bool finished = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() { finished = true; loop.quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    const bool ok = finished && reply->error() == QNetworkReply::OperationCanceledError
        && reply->bytesAvailable() <= 1025;
    reply->deleteLater();
    return ok;
}

bool boundedLogout(bool advertised) {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return false;
    AccountSession account;
    QEventLoop loop;
    bool logoutRequested = false;
    bool logoutDisconnected = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        const auto request = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request]() {
            request->append(socket->readAll());
            if (!request->contains("\r\n\r\n")) return;
            if (request->startsWith("POST /v1/auth/login ")) {
                const QByteArray body = QJsonDocument(QJsonObject{
                    {QStringLiteral("access_token"), QString(64, QChar('a'))},
                    {QStringLiteral("refresh_token"), QString(64, QChar('b'))},
                    {QStringLiteral("expires_in"), 3600}}).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            } else if (request->startsWith("POST /v1/auth/logout ")) {
                logoutRequested = request->contains("Bearer " + QByteArray(64, 'a'));
                QObject::connect(socket, &QTcpSocket::disconnected, &loop, [&]() {
                    logoutDisconnected = true;
                    loop.quit();
                });
                socket->write(advertised
                    ? "HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n"
                    : "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                socket->write(QByteArray(32768, 'x'));
                // Keep open: only the client's size bound should disconnect us.
            }
            request->clear();
        });
    });
    QObject::connect(&account, &AccountSession::changed, &loop, [&]() {
        if (account.signedIn()) account.signOut();
    });
    account.signIn(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
                   QStringLiteral("test@example.test"), QStringLiteral("test password"));
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    return logoutRequested && logoutDisconnected && !account.signedIn() && !account.busy()
        && account.accessToken().isEmpty() && account.status() == QStringLiteral("Signed out");
}

bool cancelledLogin() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return false;
    AccountSession account;
    bool accepted = false;
    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        accepted = true;
        account.signOut(); // Abort before the reply completes.
        socket->disconnectFromHost();
        socket->deleteLater();
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
    });
    account.signIn(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
                   QStringLiteral("test@example.test"), QStringLiteral("test password"));
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    return accepted && !account.signedIn() && !account.busy() && account.accessToken().isEmpty();
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!boundedResponse(true) || !boundedResponse(false) || !cancelledLogin()
        || !boundedLogout(true) || !boundedLogout(false)) {
        std::cerr << "Network response bounds or cancelled login regression\n";
        return 1;
    }
    return 0;
}
