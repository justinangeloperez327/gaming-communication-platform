#include "DesktopSettings.h"
#include "ServerProbe.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"
#include "VoiceSession.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()>& condition, int timeout = 4000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2);
    }
    return condition();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture file write");
}
QByteArray read(const QString& path) { QFile file(path); require(file.open(QIODevice::ReadOnly), "fixture read"); return file.readAll(); }
struct Api {
    QTcpServer server;
    QString mode;
    int checks = 0, logins = 0, refreshes = 0, closed = 0;
    QPointer<QTcpSocket> held;
    Api() {
        require(server.listen(QHostAddress::LocalHost), "HTTP listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, &server, [this, socket] { ++closed; socket->deleteLater(); });
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", bytes);
                    if (socket->property("answered").toBool() || !bytes.contains("\r\n\r\n")) return;
                    const auto end = bytes.indexOf("\r\n\r\n"); int length = 0;
                    for (auto line : bytes.left(end).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto path = bytes.split(' ').value(1); int code = 204; QByteArray body, extra;
                    if (path == "/ready") {
                        ++checks;
                        require(bytes.startsWith("GET ") && !bytes.toLower().contains("authorization:") &&
                                !bytes.contains("refresh_token") && !bytes.contains("password"), "probe never sends account secrets");
                        if (mode == "held") { held = socket; return; }
                        if (mode == "unavailable") code = 503;
                        if (mode == "unexpected") code = 200;
                        if (mode == "redirect") { code = 302; extra = "Location: https://redirect.invalid/ready\r\n"; }
                        if (mode == "oversized") { code = 200; body = QByteArray(32768, 'x'); }
                    } else if (path == "/v1/auth/login" || path == "/v1/auth/refresh") {
                        if (path.endsWith("login")) ++logins; else ++refreshes;
                        code = 200;
                        const auto id = mode == "wrong-user" && path.endsWith("refresh")
                            ? "00000000-0000-0000-0000-000000000002" : "00000000-0000-0000-0000-000000000001";
                        body = QJsonDocument(QJsonObject{{"access_token", QString(64, 'a')}, {"refresh_token", QString(64, 'b')},
                            {"expires_in", 900}, {"user", QJsonObject{{"id", id}, {"display_name", "Friend"}}}}).toJson();
                        if (mode == "refresh-fails" && path.endsWith("refresh")) { code = 503; body.clear(); }
                    } else if (path == "/v1/communities") { code = 200; body = "[]"; }
                    socket->write("HTTP/1.1 " + QByteArray::number(code) + " Fixture\r\n" + extra +
                        "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ~Api() { server.close(); for (auto* socket : server.findChildren<QTcpSocket*>()) { socket->disconnect(); socket->abort(); } }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};
void fileTests(const QString& path) {
    DesktopSettings settings(path);
    require(!settings.rememberServer() && settings.serverOrigin().isEmpty() && !settings.pushToTalk(), "private defaults");
    settings.saveServerOrigin("https://not-opted-in.invalid");
    require(!QFile::exists(path), "server is not stored without explicit opt-in");
    settings.saveAudio("microphone-id", "speaker-id", true);
    settings.setRememberServer(true); settings.saveServerOrigin("https://friends.invalid/");
    auto bytes = read(path); const auto json = QJsonDocument::fromJson(bytes).object();
    require(json.size() == 6 && !bytes.contains("access_token") && !bytes.contains("password") && !bytes.contains("consent"), "allowlisted preference payload only");
    DesktopSettings restored(path);
    require(restored.inputId() == "microphone-id" && restored.outputId() == "speaker-id" && restored.pushToTalk() &&
            restored.serverOrigin() == "https://friends.invalid", "restart restores only non-secret preferences");
    restored.saveAudio("bad\ninput", "output", false);
    require(read(path) == bytes, "invalid device ID cannot replace saved settings");
    for (const auto* invalid : {"http://external.invalid", "https://x.invalid/path", "https://x.invalid/?token=secret", "https://user:password@x.invalid"}) {
        restored.saveServerOrigin(invalid); require(read(path) == bytes, "unsafe saved server rejected");
    }
    restored.setRememberServer(false);
    require(!read(path).contains("friends.invalid") && DesktopSettings(path).serverOrigin().isEmpty(), "forget removes persisted server");
    for (const auto& bad : {QByteArray("{"), QByteArray(20000, 'x'), QByteArray("{\"version\":99}")}) {
        write(path, bad); DesktopSettings corrupt(path);
        require(corrupt.inputId().isEmpty() && !corrupt.status().isEmpty(), "damaged/newer settings use defaults with guidance");
        corrupt.saveAudio("new", "output", true); require(read(path) == bad, "unreadable settings preserved until explicit reset");
        corrupt.reset(); require(QJsonDocument::fromJson(read(path)).object().value("version").toInt() == 1, "explicit reset repairs settings");
    }
    const auto blocker = path + ".blocker"; write(blocker, "file"); DesktopSettings denied(blocker + "/preferences.json");
    denied.saveAudio("input", "output", true);
    require(denied.inputId() == "input" && denied.status().contains("could not be saved"), "write failure keeps current choices with guidance");
}
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir folder; require(folder.isValid(), "isolated profile");
        fileTests(folder.filePath("files.json"));
        Api api; ServerProbe probe(nullptr, 300);
        for (const auto* invalid : {"", "http://external.invalid", "https://x.invalid/path", "https://x.invalid/?secret=x", "https://user:secret@x.invalid"}) {
            probe.check(invalid); require(!probe.busy() && probe.result() == "invalid" && api.checks == 0, "unsafe probe origin rejected before I/O");
        }
        const auto check = [&](const QString& mode, const QString& result) {
            api.mode = mode; const auto before = api.checks; probe.check(api.origin());
            require(waitFor([&] { return !probe.busy(); }) && probe.result() == result && api.checks == before + 1, "bounded readiness outcome without retries");
        };
        check({}, "ready"); check("unavailable", "not_ready"); check("unexpected", "unexpected");
        check("redirect", "redirect"); check("oversized", "unreachable"); check("held", "unreachable");
        api.mode = "held"; probe.check(api.origin()); require(waitFor([&] { return api.held && probe.busy(); }), "pending probe");
        probe.clear(); require(probe.result() == "idle" && !probe.busy(), "cancel clears pending probe");
        check({}, "ready");
        auto transient = std::make_unique<ServerProbe>(); api.mode = "held"; const auto before = api.checks;
        transient->check(api.origin()); require(waitFor([&] { return api.checks == before + 1; }), "probe before destruction");
        const auto closed = api.closed; transient.reset(); require(waitFor([&] { return api.closed > closed; }), "destruction aborts peer request");

        DesktopSettings settings(folder.filePath("ui.json")); AccountSession account(nullptr, &settings);
        ChannelDirectory directory(&account);
        VoiceSession voice(&account, &directory, nullptr, {}, &settings);
        QQmlEngine engine; int warnings = 0;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError>& errors) {
            warnings += errors.size(); for (const auto& error : errors) std::cerr << error.toString().toStdString() << '\n';
        });
        engine.rootContext()->setContextProperty("prefs", &settings); engine.rootContext()->setContextProperty("login", &account);
        engine.rootContext()->setContextProperty("rooms", &directory); engine.rootContext()->setContextProperty("voiceControl", &voice);
        engine.rootContext()->setContextProperty("check", &probe);
        const auto component = [&](const QByteArray& text, const char* file) {
            QQmlComponent value(&engine); value.setData(text, QUrl::fromLocalFile(QStringLiteral(GCP_SETTINGS_QML_DIR) + "/" + QString::fromLatin1(file)));
            auto result = std::unique_ptr<QObject>(value.create()); if (!result) std::cerr << value.errorString().toStdString();
            require(result != nullptr, "shipped settings/account panel loads"); return result;
        };
        auto accountPanel = component("import QtQuick\nimport \".\"\nAccountPanel { account: login; preferences: prefs; probe: check; width: 620 }", "AccountHarness.qml");
        auto panel = component("import QtQuick\nimport \".\"\nSettingsPanel { preferences: prefs; voice: voiceControl; account: login; directory: rooms; expanded: true; width: 620 }", "SettingsHarness.qml");
        const auto click = [&](QObject* root, const char* name) {
            auto* field = root->findChild<QObject*>(QString::fromLatin1(name));
            require(field && field->property("enabled").toBool() && QMetaObject::invokeMethod(field, "clicked"), "enabled shipped QML action");
        };
        accountPanel->findChild<QObject*>("accountOrigin")->setProperty("text", api.origin());
        click(accountPanel.get(), "accountCheckConnection");
        require(waitFor([&] { return !probe.busy(); }), "QML readiness check deadline");
        api.mode.clear();
        auto* remember = accountPanel->findChild<QObject*>("accountRememberServer");
        remember->setProperty("checked", true); require(QMetaObject::invokeMethod(remember, "toggled"), "remember opt-in");
        account.signIn(api.origin(), "test@example.test", "private-test-password");
        require(waitFor([&] { return account.signedIn() && !account.busy(); }), "actual account login");
        require(settings.serverOrigin() == api.origin() && settings.rememberServer(), "server saved only after successful account response");
        const auto saved = read(folder.filePath("ui.json"));
        require(!saved.contains("private-test-password") && !saved.contains("test@example.test") && !saved.contains(QString(64, 'a').toUtf8()), "login secrets and email absent from disk");
        click(panel.get(), "settingsCheckSession");
        require(waitFor([&] { return !account.busy(); }) && account.signedIn() && api.refreshes == 1, "manual session check rotates once");
        api.mode = "wrong-user"; click(panel.get(), "settingsCheckSession");
        require(waitFor([&] { return !account.busy(); }) && !account.signedIn() && account.reauthenticationRequired(), "changed refresh principal rejected and reauthentication required");
        api.mode.clear(); account.signIn(api.origin(), "test@example.test", "private-test-password");
        require(waitFor([&] { return account.signedIn() && !account.busy(); }), "explicit recovery sign-in");
        api.mode = "refresh-fails"; const auto count = api.refreshes; click(panel.get(), "settingsCheckSession");
        require(waitFor([&] { return !account.busy(); }) && api.refreshes == count + 1 && !account.signedIn() &&
                account.status().contains("Sign in again") && !voice.busy(), "failed refresh clears login, never retries or rejoins");
        click(panel.get(), "settingsForgetServer"); require(settings.serverOrigin().isEmpty(), "forget saved server via shipped control");
        click(panel.get(), "settingsReset");
        require(!settings.rememberServer() && settings.inputId().isEmpty() && !settings.pushToTalk() && voice.muted(), "reset keeps media consent disarmed");
        require(warnings == 0, "settings/account QML has no warnings");
        std::cout << "Preference persistence, privacy, corrupt/unwritable recovery, bounded server probe and shipped QML/session recovery passed; no physical devices/media.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
