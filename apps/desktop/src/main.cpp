#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
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

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection
    );

    engine.loadFromModule("Gcp.Desktop", "Main");
    return app.exec();
}
