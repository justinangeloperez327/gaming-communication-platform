#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <iostream>
#include <memory>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(GCP_AUDIO_PICKER_PATH)));
    const std::unique_ptr<QObject> picker(component.create());
    if (!picker) {
        std::cerr << component.errorString().toStdString();
        return 1;
    }
    const QVariantMap first{{"name", "First"}, {"id", "first"}};
    const QVariantMap second{{"name", "Second"}, {"id", "second"}};
    const auto selected = [&](int index, const QString &id) {
        QCoreApplication::processEvents();
        return picker->property("currentIndex").toInt() == index
            && picker->property("currentValue").toString() == id;
    };
    picker->setProperty("devices", QVariantList{first, second});
    if (!selected(0, {})) return 2;
    picker->setProperty("selectedDeviceId", QStringLiteral("second"));
    if (!selected(2, QStringLiteral("second"))) return 3;
    // Model replacement/reordering must follow identity rather than old index.
    picker->setProperty("devices", QVariantList{second, first});
    if (!selected(1, QStringLiteral("second"))) return 4;
    picker->setProperty("devices", QVariantList{first});
    if (!selected(1, QStringLiteral("second")) || !picker->property("displayText").toString().contains("unavailable")) return 16;
    picker->setProperty("selectedDeviceId", QString{});
    if (!selected(0, {})) return 5;
    picker->setProperty("devices", QVariantList{first, second});
    picker->setProperty("selectedDeviceId", QStringLiteral("first"));
    if (!selected(1, QStringLiteral("first"))) return 6;
    picker->setProperty("selectedDeviceId", QString{});
    if (!selected(0, {})) return 7;
    QQmlComponent directoryComponent(&engine, QUrl::fromLocalFile(QStringLiteral(GCP_ID_PICKER_PATH)));
    const std::unique_ptr<QObject> directory(directoryComponent.create());
    if (!directory) {
        std::cerr << directoryComponent.errorString().toStdString();
        return 8;
    }
    const auto directorySelected = [&](int index, const QString &id) {
        QCoreApplication::processEvents();
        return directory->property("currentIndex").toInt() == index
            && directory->property("currentValue").toString() == id;
    };
    directory->setProperty("emptyLabel", QStringLiteral("Choose a voice channel"));
    directory->setProperty("options", QVariantList{first, second});
    if (!directorySelected(-1, {}) ||
        directory->property("displayText").toString() != QStringLiteral("Choose a voice channel")) return 9;
    directory->setProperty("selectedId", QStringLiteral("second"));
    if (!directorySelected(1, QStringLiteral("second"))) return 10;
    directory->setProperty("options", QVariantList{second, first});
    if (!directorySelected(0, QStringLiteral("second"))) return 11;
    directory->setProperty("selectedId", QString{});
    if (!directorySelected(-1, {})) return 12;
    directory->setProperty("options", QVariantList{});
    if (!directorySelected(-1, {})) return 13;
    directory->setProperty("options", QVariantList{first, second});
    if (!directorySelected(-1, {})) return 14;
    directory->setProperty("selectedId", QStringLiteral("missing"));
    if (!directorySelected(-1, {})) return 15;
    std::cout << "QML audio picker identity and fallback tests passed\n";
}
