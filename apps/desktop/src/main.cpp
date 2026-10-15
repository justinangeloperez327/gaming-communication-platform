#include "VoiceSession.h"
#include "AccountSession.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>

#include <cstdlib>
#include <string_view>

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--self-test") {
            return EXIT_SUCCESS;
        }
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Gaming Communication Platform"));
    QCoreApplication::setOrganizationName(QStringLiteral("GCP"));

    AccountSession account;
    VoiceSession voice(&account);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("voiceSession"), &voice);
    engine.rootContext()->setContextProperty(QStringLiteral("accountSession"), &account);
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
    return app.exec();
}
