#include "VoiceSession.h"
#include "AccountSession.h"
#include "ChannelDirectory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
bool permittedUrl(const QUrl &url) {
    if (!url.isValid() || url.userInfo().size() || url.hasFragment() || url.hasQuery()) return false;
    const QString host = url.host().toLower();
    const bool loopback = host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1");
    return url.scheme() == QStringLiteral("https") || (loopback && url.scheme() == QStringLiteral("http"));
}
bool uuidValid(const QString &value) {
    return QUuid::fromString(value).isNull() == false;
}
}

VoiceSession::VoiceSession(AccountSession *account, ChannelDirectory *directory, QObject *parent) : QObject(parent), account_(account), directory_(directory) {
    deviceScanWatchdog_.setSingleShot(true);
    idleDeviceRefresh_.setInterval(30000);
    connect(&idleDeviceRefresh_, &QTimer::timeout, this, &VoiceSession::scanDevices);
    idleDeviceRefresh_.start();
    connect(&deviceScanWatchdog_, &QTimer::timeout, this, [this]() {
        if (scanning()) devices_.kill();
    });
    connect(&devices_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        deviceScanWatchdog_.stop();
        const QByteArray data = devices_.readAllStandardOutput();
        const QByteArray err = devices_.readAllStandardError();
        Q_UNUSED(err);
        QVariantList inputs, outputs;
        if (code == 0 && exit == QProcess::NormalExit && data.size() <= 65536) {
            for (const QByteArray &line : data.split('\n')) {
                const bool input = line.startsWith("input [");
                if (!input && !line.startsWith("output [")) continue;
                const int split = line.indexOf(" | id=");
                const int close = line.indexOf("] ");
                if (split <= close || close < 0) continue;
                const QString name = QString::fromUtf8(line.mid(close + 2, split - close - 2)).trimmed();
                const QString id = QString::fromUtf8(line.mid(split + 6)).trimmed();
                if (name.isEmpty() || name.size() > 256 || id.isEmpty() || id.size() > 512) continue;
                QVariantMap item{{QStringLiteral("name"), name}, {QStringLiteral("id"), id}};
                if (input) inputs.append(item); else outputs.append(item);
            }
        }
        if (code != 0 || exit != QProcess::NormalExit || data.size() > 65536) {
            // Failed scans must not erase a previously selected device.
            setStatus(QStringLiteral("Audio device scan failed; retry manually"));
            emit devicesChanged();
            return;
        }
        inputs_ = inputs;
        outputs_ = outputs;
        bool foundInput = false;
        for (const auto &item : inputs_)
            if (item.toMap().value(QStringLiteral("id")).toString() == inputId_) foundInput = true;
        if (!foundInput) inputId_.clear();
        bool foundOutput = false;
        for (const auto &item : outputs_) if (item.toMap().value("id").toString() == outputId_) foundOutput = true;
        if (!foundOutput) outputId_.clear();
        emit devicesChanged();
    });
    connect(&devices_, &QProcess::started, this, [this]() {
        deviceScanWatchdog_.start(6000);
        emit devicesChanged();
    });
    startupWatchdog_.setSingleShot(true);
    stopWatchdog_.setSingleShot(true);
    connectWatchdog_.setSingleShot(true);
    connect(&connectWatchdog_, &QTimer::timeout, this, [this]() {
        if (running() && !connected_ && !stopRequested_) {
            // A running process is not evidence of room authorization or connectivity.
            connectionTimedOut_ = true;
            setStatus(QStringLiteral("Voice room connection timed out; stopping"));
            stopRequested_ = true;
            process_.terminate();
            stopWatchdog_.start(2000);
        }
    });
    connect(&startupWatchdog_, &QTimer::timeout, this, [this]() {
        if (process_.state() == QProcess::Starting) {
            stopRequested_ = true;
            setStatus(QStringLiteral("Native voice startup timed out"));
            process_.kill();
        }
    });
    connect(&stopWatchdog_, &QTimer::timeout, this, [this]() {
        if (running()) process_.kill();
    });
    connect(directory_, &ChannelDirectory::selectionChanged, this, &VoiceSession::leave);
    connect(account_, &AccountSession::signedOut, this, &VoiceSession::leave);
    connect(&process_, &QProcess::started, this, [this]() {
        startupWatchdog_.stop();
        connectWatchdog_.start(22000);
        process_.setProcessEnvironment(QProcessEnvironment());
        setStatus(microphoneEnabled_ ? QStringLiteral("Native voice process started · microphone enabled")
                                    : QStringLiteral("Native voice process started · listen only"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        startupWatchdog_.stop();
        stopWatchdog_.stop();
        connectWatchdog_.stop();
        process_.setProcessEnvironment(QProcessEnvironment());
        const bool intentional = stopRequested_;
        stopRequested_ = false;
        microphoneEnabled_ = false;
        connected_ = false;
        outputBuffer_.clear();
        const bool deviceLost = deviceLost_;
        deviceLost_ = false;
        const bool timedOut = connectionTimedOut_;
        connectionTimedOut_ = false;
        setStatus(deviceLost ? QStringLiteral("Audio device removed; rescan and join manually") :
                  timedOut ? QStringLiteral("Room connection timed out; retry manually") :
                  intentional ? QStringLiteral("Disconnected") :
                  exitStatus == QProcess::NormalExit && exitCode == 0
                      ? QStringLiteral("Disconnected")
                      : QStringLiteral("Voice process stopped; connection not verified"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            startupWatchdog_.stop();
            connectWatchdog_.stop();
            process_.setProcessEnvironment(QProcessEnvironment());
            microphoneEnabled_ = false;
            connected_ = false;
            outputBuffer_.clear();
            setStatus(QStringLiteral("Cannot start native voice executable"));
            emit runningChanged();
        }
    });
    // Never log child output or backend tokens.
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        const QByteArray chunk = process_.readAllStandardOutput();
        // Never print stdout: it may include diagnostic information. Parse only
        // complete fixed-format status lines; bound buffered memory.
        if (chunk.size() > 8192 || outputBuffer_.size() + chunk.size() > 8192) {
            outputBuffer_.clear();
            return;
        }
        outputBuffer_.append(chunk);
        while (true) {
            const auto newline = outputBuffer_.indexOf('\n');
            if (newline < 0) break;
            const QByteArray line = outputBuffer_.left(newline).trimmed();
            outputBuffer_.remove(0, newline + 1);
            if (line == "GCP_VOICE_EVENT:connected" && running() && !stopRequested_) {
                connected_ = true;
                connectWatchdog_.stop();
                setStatus(QStringLiteral("Connected to LiveKit room (audio not verified)"));
            } else if (line == "GCP_VOICE_EVENT:reconnecting" && connected_ && !stopRequested_) {
                connected_ = false;
                setStatus(QStringLiteral("Network interrupted · SDK reconnecting"));
                connectWatchdog_.start(15000);
            } else if (line == "GCP_VOICE_EVENT:reconnected" && !stopRequested_ && running()) {
                connected_ = true;
                connectWatchdog_.stop();
                setStatus(QStringLiteral("LiveKit reconnected (audio not verified)"));
            } else if (line == "GCP_VOICE_EVENT:device_lost" && !stopRequested_) {
                connected_ = false;
                deviceLost_ = true;
                stopRequested_ = true;
                setStatus(QStringLiteral("Selected audio device disconnected; stopping voice"));
                connectWatchdog_.stop();
                process_.terminate();
                stopWatchdog_.start(2000);
            } else if (line == "GCP_VOICE_EVENT:connection_lost" && !stopRequested_) {
                connected_ = false;
                setStatus(QStringLiteral("LiveKit connection lost · stopping voice"));
                stopRequested_ = true;
                connectWatchdog_.stop();
                process_.terminate();
                stopWatchdog_.start(2000);
            } else if (line == "GCP_VOICE_EVENT:disconnected") {
                connected_ = false;
                setStatus(QStringLiteral("Disconnected from LiveKit room"));
            }
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this]() { process_.readAllStandardError(); });
}

bool VoiceSession::available() const {
#ifdef Q_OS_WIN
    return QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
#else
    return false;
#endif
}

void VoiceSession::scanDevices() {
    if (busy() || scanning() || !available()) return;
    devices_.setProgram(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
    devices_.setArguments({QStringLiteral("devices")});
    devices_.start();

}
void VoiceSession::selectInput(const QString &id) {
    if (busy()) return;
    if (id.isEmpty()) { inputId_.clear(); emit devicesChanged(); return; }
    for (const auto &item : inputs_)
        if (item.toMap().value(QStringLiteral("id")).toString() == id) {
            inputId_ = id; emit devicesChanged(); return;
        }
}
void VoiceSession::selectOutput(const QString &id) {
    if (busy()) return;
    if (id.isEmpty()) { outputId_.clear(); emit devicesChanged(); return; }
    for (const auto &item : outputs_)
        if (item.toMap().value(QStringLiteral("id")).toString() == id) {
            outputId_ = id; emit devicesChanged(); return;
        }
}

void VoiceSession::setStatus(const QString &value) {
    if (status_ != value) { status_ = value; emit statusChanged(); }
}

void VoiceSession::start(bool publishMicrophone) {
    if (busy() || scanning()) return;
    if (!available()) { setStatus(QStringLiteral("Native voice executable is missing")); return; }
    const QUrl api = account_->apiOrigin();
    const auto access = account_->accessToken();
    const auto community = directory_->communityId();
    const auto channel = directory_->channelId();
    if (!permittedUrl(api) || access.size() != 64 || !uuidValid(community) || !uuidValid(channel)) {
        setStatus(QStringLiteral("Sign in and select an authorized voice channel"));
        return;
    }
    for (const QChar c : access) {
        if (!c.isDigit() && (c < QChar('a') || c > QChar('f'))) {
            setStatus(QStringLiteral("Invalid application access token"));
            return;
        }
    }
    QUrl endpoint = api;
    QString base = endpoint.path();
    if (!base.isEmpty() && base != QStringLiteral("/")) {
        setStatus(QStringLiteral("API server origin must not contain a path"));
        return;
    }
    endpoint.setPath(QStringLiteral("/v1/communities/%1/channels/%2/livekit-token").arg(community, channel));
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    setStatus(QStringLiteral("Requesting authorized voice room…"));
    pending_ = network_.post(request, QByteArray("{}"));
    emit runningChanged();
    QNetworkReply *reply = pending_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, publishMicrophone]() {
        // Cancellation takes precedence over completed replies.
        if (pending_ != reply) { reply->deleteLater(); return; }
        pending_ = nullptr;
        emit runningChanged();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        QByteArray response = reply->readAll();
        reply->deleteLater();
        if (error != QNetworkReply::NoError || status != 201 || response.size() > 16384) {
            setStatus(QStringLiteral("Voice authorization failed or expired"));
            return;
        }
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(response, &parse);
        response.fill(0);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            setStatus(QStringLiteral("Invalid voice authorization response"));
            return;
        }
        const auto obj = doc.object();
        const QString server = obj.value(QStringLiteral("server_url")).toString();
        const QString token = obj.value(QStringLiteral("participant_token")).toString();
        const int ttl = obj.value(QStringLiteral("expires_in")).toInt();
        const QUrl voiceUrl(server);
        if ((voiceUrl.scheme() != QStringLiteral("wss") &&
             !(voiceUrl.scheme() == QStringLiteral("ws") &&
               (voiceUrl.host() == QStringLiteral("localhost") || voiceUrl.host() == QStringLiteral("127.0.0.1"))))
            || !voiceUrl.userInfo().isEmpty() || !voiceUrl.isValid() ||
            token.size() < 50 || token.size() > 8192 || ttl < 15 || ttl > 300) {
            setStatus(QStringLiteral("Rejected unsafe voice room credentials"));
            return;
        }
        launch(publishMicrophone, server, token, ttl);
    });
}

void VoiceSession::launch(bool publishMicrophone, const QString &url, const QString &token, int lifetime) {
    if (running()) return;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LIVEKIT_URL"), url);
    env.insert(QStringLiteral("LIVEKIT_TOKEN"), token);
    env.remove(QStringLiteral("GCP_ACCESS_TOKEN"));
    env.remove(QStringLiteral("GCP_REFRESH_TOKEN"));
    stopRequested_ = false;
    connectionTimedOut_ = false;
    deviceLost_ = false;
    connected_ = false;
    outputBuffer_.clear();
    microphoneEnabled_ = publishMicrophone;
    process_.setProcessEnvironment(env);
    process_.setProgram(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
    QStringList args{QStringLiteral("join"), QStringLiteral("--seconds"), QString::number(qMin(lifetime, 300))};
    if (!outputId_.isEmpty()) args << QStringLiteral("--output") << outputId_;
    if (publishMicrophone && !inputId_.isEmpty()) args << QStringLiteral("--input") << inputId_;
    if (publishMicrophone) {
        args << QStringLiteral("--publish-mic") << QStringLiteral("--confirm-microphone") << QStringLiteral("yes");
    }
    process_.setArguments(args);
    setStatus(QStringLiteral("Starting native voice…"));
    process_.start();
    startupWatchdog_.start(12000);
    emit runningChanged();
}

void VoiceSession::joinListenOnly() { start(false); }
void VoiceSession::joinWithMicrophone(bool consent) {
    if (!consent) { setStatus(QStringLiteral("Explicit microphone consent required")); return; }
    start(true);
}
void VoiceSession::leave() {
    if (pending_) {
        auto *reply = pending_;
        pending_ = nullptr;
        reply->abort();
        emit runningChanged();
    }
    if (!running()) { setStatus(QStringLiteral("Disconnected")); return; }
    stopRequested_ = true;
    connected_ = false;
    startupWatchdog_.stop();
    connectWatchdog_.stop();
    setStatus(QStringLiteral("Disconnecting…"));
    process_.terminate();
    stopWatchdog_.start(2000);
}
