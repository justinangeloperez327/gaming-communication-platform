#include "DesktopSettings.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>

// Test-only executable, excluded from every distribution allowlist. It reads the
// production settings location without initializing GUI, audio, media or HTTP.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Gaming Communication Platform"));
    QCoreApplication::setOrganizationName(QStringLiteral("GCP"));
    const auto arguments = app.arguments();
    const auto path = DesktopSettings::defaultFileName();
    if (arguments.size() != 2 || path.isEmpty()) return EXIT_FAILURE;
    if (arguments[1] == QStringLiteral("--path")) {
        std::cout << path.toUtf8().constData() << '\n';
        return EXIT_SUCCESS;
    }
    if (arguments[1] != QStringLiteral("--verify-fixture")) return EXIT_FAILURE;
    const DesktopSettings settings(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) return EXIT_FAILURE;
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    if (json.size() != 6 || !settings.status().isEmpty() ||
        settings.inputId() != QStringLiteral("fixture-input") ||
        settings.outputId() != QStringLiteral("fixture-output") ||
        !settings.pushToTalk() || !settings.rememberServer() ||
        settings.serverOrigin() != QStringLiteral("https://settings-fixture.invalid")) {
        std::cerr << "Installed preference fixture did not survive or load.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Production preference path and fixture verified; no credentials or media state restored.\n";
    return EXIT_SUCCESS;
}
