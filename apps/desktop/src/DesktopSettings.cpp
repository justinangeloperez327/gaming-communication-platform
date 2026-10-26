#include "DesktopSettings.h"
#include "ChatProtocol.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <utility>

namespace {
constexpr qint64 settingsLimit = 16384;
bool validOrigin(const QString& value) {
    return value.size() <= 2048 && ChatProtocol::originValid(QUrl(value));
}
}
DesktopSettings::DesktopSettings(QString fileName, QObject* parent)
    : QObject(parent), fileName_(std::move(fileName)) { load(); }
QString DesktopSettings::defaultFileName() {
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return folder.isEmpty() ? QString{} : QDir(folder).filePath(QStringLiteral("preferences.json"));
}
bool DesktopSettings::validDeviceId(const QString& id) {
    if (id.size() > 512) return false;
    for (const auto c : id) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
void DesktopSettings::load() {
    if (fileName_.isEmpty()) { status_ = QStringLiteral("Preferences are temporary for this run."); return; }
    QFile file(fileName_);
    if (!file.exists()) return;
    // Preserve corrupt/newer data until an explicit Reset instead of silently
    // overwriting it on the next unrelated checkbox change.
    writable_ = false;
    status_ = QStringLiteral("Saved settings could not be read. Defaults are in use; Reset saved settings to replace them.");
    if (!file.open(QIODevice::ReadOnly) || file.size() > settingsLimit) return;
    const auto bytes = file.read(settingsLimit + 1);
    if (bytes.size() > settingsLimit || file.error() != QFileDevice::NoError) return;
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) return;
    const auto json = doc.object();
    if (json.value("version") != QJsonValue(1) || !json.value("remember_server").isBool() ||
        !json.value("push_to_talk").isBool() || !json.value("input_id").isString() ||
        !json.value("output_id").isString() || !json.value("server_origin").isString()) return;
    const auto input = json.value("input_id").toString(), output = json.value("output_id").toString();
    const auto server = json.value("server_origin").toString();
    const bool remember = json.value("remember_server").toBool();
    if (!validDeviceId(input) || !validDeviceId(output) ||
        (!server.isEmpty() && (!remember || !validOrigin(server)))) return;
    inputId_ = input; outputId_ = output; rememberServer_ = remember;
    serverOrigin_ = server; pushToTalk_ = json.value("push_to_talk").toBool();
    writable_ = true; status_.clear();
}
void DesktopSettings::save() {
    if (fileName_.isEmpty()) { emit changed(); return; }
    if (!writable_) { emit changed(); return; }
    const auto folder = QFileInfo(fileName_).absolutePath();
    QSaveFile file(fileName_);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"remember_server", rememberServer_},
        {"server_origin", serverOrigin_}, {"input_id", inputId_}, {"output_id", outputId_},
        {"push_to_talk", pushToTalk_}}).toJson(QJsonDocument::Compact);
    if (!QDir().mkpath(folder) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit())
        status_ = QStringLiteral("Settings could not be saved. Choices still apply for this run; check your Windows user profile permissions.");
    else status_.clear();
    emit changed();
}
void DesktopSettings::setRememberServer(bool remember) {
    if (rememberServer_ == remember) return;
    rememberServer_ = remember;
    if (!remember) serverOrigin_.clear();
    save();
}
void DesktopSettings::saveServerOrigin(const QString& origin) {
    if (!rememberServer_ || !validOrigin(origin)) return;
    QUrl url(origin); url.setPath({});
    const auto normalized = url.toString();
    if (serverOrigin_ == normalized) return;
    serverOrigin_ = normalized; save();
}
void DesktopSettings::saveAudio(const QString& input, const QString& output, bool pushToTalk) {
    if (!validDeviceId(input) || !validDeviceId(output) ||
        (inputId_ == input && outputId_ == output && pushToTalk_ == pushToTalk)) return;
    inputId_ = input; outputId_ = output; pushToTalk_ = pushToTalk; save();
}
void DesktopSettings::reset() {
    inputId_.clear(); outputId_.clear(); serverOrigin_.clear();
    rememberServer_ = pushToTalk_ = false; writable_ = true; save();
}
