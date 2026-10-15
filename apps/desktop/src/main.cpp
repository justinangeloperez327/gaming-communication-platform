#include "VoiceSession.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>
#include <QTimer>

#include <cstdlib>
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
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Gaming Communication Platform"));
    QCoreApplication::setOrganizationName(QStringLiteral("GCP"));

    AccountSession account;
    ChannelDirectory directory(&account);
    VoiceSession voice(&account, &directory);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("voiceSession"), &voice);
    engine.rootContext()->setContextProperty(QStringLiteral("accountSession"), &account);
    engine.rootContext()->setContextProperty(QStringLiteral("channelDirectory"), &directory);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection
    );
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
