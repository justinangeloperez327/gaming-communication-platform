#include "VoiceSession.h"
#include "ScreenSession.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "ChatSession.h"
#include "FriendsDirectory.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>
#include <QTimer>

#include <cstdlib>
#include <cstdio>
#include <string_view>

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--self-test") {
            return EXIT_SUCCESS;
        }
    }

    bool smokeQml = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--smoke-qml") smokeQml = true;
    }
    if (smokeQml) {
        // GUI-subsystem applications otherwise send Qt diagnostics only to the
        // Windows debugger. CTest can capture this startup-only stderr handler.
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        });
    }
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Gaming Communication Platform"));
    QCoreApplication::setOrganizationName(QStringLiteral("GCP"));

    AccountSession account;
    ChannelDirectory directory(&account);
    VoiceSession voice(&account, &directory);
    ScreenSession screen(&account, &directory);
    FriendsDirectory friends(&account);
    ChatSession chat(&account, &directory);
    ChatSession directChat(&account, &friends);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("voiceSession"), &voice);
    engine.rootContext()->setContextProperty(QStringLiteral("screenSession"), &screen);
    engine.rootContext()->setContextProperty(QStringLiteral("accountSession"), &account);
    engine.rootContext()->setContextProperty(QStringLiteral("channelDirectory"), &directory);
    engine.rootContext()->setContextProperty(QStringLiteral("chatSession"), &chat);
    engine.rootContext()->setContextProperty(QStringLiteral("friendsDirectory"), &friends);
    engine.rootContext()->setContextProperty(QStringLiteral("directChatSession"), &directChat);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection
    );
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &screen, &ScreenSession::stop);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &voice, &VoiceSession::leave);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &account, &AccountSession::signOut);
    engine.loadFromModule("Gcp.Desktop", "Main");
    if (smokeQml) {
        // Exercise actual QML module and plugin loading, unlike --self-test.
        // A missing module triggers objectCreationFailed and exits nonzero.
        QTimer::singleShot(2500, &app, [&app, &engine]() {
            app.exit(engine.rootObjects().isEmpty() ? EXIT_FAILURE : EXIT_SUCCESS);
        });
    }
    return app.exec();
}
