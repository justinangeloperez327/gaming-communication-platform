#include "BoundedReply.h"
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
#include <utility>

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

VoiceSession::VoiceSession(AccountSession *account, ChannelDirectory *directory, QObject *parent, VoiceRuntime runtime) : QObject(parent), runtime_(std::move(runtime)), account_(account), directory_(directory) {
    heartbeatTimer_.setInterval(1000);
    authorizationTimer_.setSingleShot(true);
    authorizationWatchdog_.setSingleShot(true);
    connect(&authorizationWatchdog_, &QTimer::timeout, this, [this]() {
        stopWithReason(QStringLiteral("Voice authorization deadline exceeded; join again manually"));
    });
    muteWatchdog_.setSingleShot(true);
    connect(&heartbeatTimer_, &QTimer::timeout, this, [this]() {
        if (!sendControl("ping\n")) stopWithReason(QStringLiteral("Voice control pipe failed"));
    });
    connect(&authorizationTimer_, &QTimer::timeout, this, [this]() {
        if (running() && !stopRequested_) requestAuthorization(microphoneEnabled_, true);
    });
    connect(&muteWatchdog_, &QTimer::timeout, this, [this]() {
        stopWithReason(QStringLiteral("Microphone mute was not acknowledged; stopping voice"));
    });
    deviceScanWatchdog_.setSingleShot(true);
    idleDeviceRefresh_.setInterval(30000);
    connect(&idleDeviceRefresh_, &QTimer::timeout, this, [this]() {
        if (account_->signedIn()) scanDevices();
    });
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
    connect(&devices_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            deviceScanWatchdog_.stop();
            setStatus(QStringLiteral("Cannot start audio device scan; retry manually"));
            emit devicesChanged();
        }
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
        if (!sendControl("ping\n")) {
            stopWithReason(QStringLiteral("Cannot initialize voice control pipe"));
            return;
        }
        heartbeatTimer_.start();
        setStatus(microphoneEnabled_ ? QStringLiteral("Native voice process started · microphone starts muted")
                                    : QStringLiteral("Native voice process started · listen only"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        resetControls();
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
        const auto reason = stopReason_;
        stopReason_.clear();
        setStatus(!reason.isEmpty() ? reason : deviceLost ? QStringLiteral("Audio device removed; rescan and join manually") :
                  timedOut ? QStringLiteral("Room connection timed out; retry manually") :
                  intentional ? QStringLiteral("Disconnected") :
                  exitStatus == QProcess::NormalExit && exitCode == 0
                      ? QStringLiteral("Disconnected")
                      : QStringLiteral("Voice process stopped; connection not verified"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            resetControls();
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
            if (line == "GCP_VOICE_EVENT:muted") {
                transmitting_ = false;
                muteWatchdog_.stop();
                emit controlsChanged();
            } else if (line == "GCP_VOICE_EVENT:unmuted" && !stopRequested_) {
                transmitting_ = true;
                emit controlsChanged();
            } else if (line == "GCP_VOICE_EVENT:connected" && running() && !stopRequested_) {
                connected_ = true;
                connectWatchdog_.stop();
                if (!authorizationTimer_.isActive() && !pending_) authorizationTimer_.start(qMax(100, runtime_.authorizationIntervalMs));
                setStatus(QStringLiteral("Connected to LiveKit room (audio not verified)"));
            } else if (line == "GCP_VOICE_EVENT:reconnecting" && connected_ && !stopRequested_) {
                connected_ = false;
                setStatus(QStringLiteral("Network interrupted · SDK reconnecting"));
                connectWatchdog_.start(15000);
            } else if (line == "GCP_VOICE_EVENT:reconnected" && !stopRequested_ && running()) {
                connected_ = true;
                connectWatchdog_.stop();
                if (!authorizationTimer_.isActive() && !pending_) authorizationTimer_.start(qMax(100, runtime_.authorizationIntervalMs));
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

VoiceSession::~VoiceSession() {
    // Destruction must not leave microphone capture in an orphan child. There is
    // no event loop left to deliver the asynchronous leave watchdog here.
    for (auto *child : {&process_, &devices_}) {
        child->disconnect(this);
        if (child->state() != QProcess::NotRunning) {
            child->kill();
            child->waitForFinished(2000);
        }
    }
}

QString VoiceSession::nativeProgram() const {
    if (!runtime_.program.isEmpty()) return runtime_.program;
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe"));
}

bool VoiceSession::available() const {
    if (!runtime_.program.isEmpty()) return QFileInfo::exists(runtime_.program);
#ifdef Q_OS_WIN
    return QFileInfo::exists(nativeProgram());
#else
    return false;
#endif
}

void VoiceSession::scanDevices() {
    if (busy() || scanning() || !available()) return;
    devices_.setProgram(nativeProgram());
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
    requestAuthorization(publishMicrophone, false);
}

void VoiceSession::requestAuthorization(bool publishMicrophone, bool recheck) {
    if (pending_ || (recheck && (!running() || stopRequested_))) return;
    if (!account_->signedIn()) {
        stopWithReason(QStringLiteral("Sign-in required for voice"));
        return;
    }
    const QUrl api = account_->apiOrigin();
    const auto access = account_->accessToken();
    const auto community = directory_->communityId();
    const auto channel = directory_->channelId();
    if (!permittedUrl(api) || access.size() != 64 || !uuidValid(community) || !uuidValid(channel)) {
        if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
        else setStatus(QStringLiteral("Sign in and select an authorized voice channel"));
        return;
    }
    for (const QChar c : access) {
        if (!c.isDigit() && (c < QChar('a') || c > QChar('f'))) {
            if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
            else setStatus(QStringLiteral("Invalid application access token"));
            return;
        }
    }
    QUrl endpoint = api;
    QString base = endpoint.path();
    if (!base.isEmpty() && base != QStringLiteral("/")) {
        if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
        else setStatus(QStringLiteral("API server origin must not contain a path"));
        return;
    }
    endpoint.setPath(QStringLiteral("/v1/communities/%1/channels/%2/livekit-token").arg(community, channel));
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    if (!recheck) setStatus(QStringLiteral("Requesting authorized voice room…"));
    pending_ = network_.post(request, QByteArray("{}"));
    boundReply(pending_, 16384);
    authorizationWatchdog_.start(10000);
    emit runningChanged();
    QNetworkReply *reply = pending_;
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, publishMicrophone, recheck, generation]() {
        // Cancellation takes precedence over completed replies.
        if (pending_ != reply || generation != generation_) { reply->deleteLater(); return; }
        pending_ = nullptr;
        authorizationWatchdog_.stop();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        QByteArray response = reply->readAll();
        reply->deleteLater();
        if (error != QNetworkReply::NoError || status != 201 || response.size() > 16384) {
            if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
            else setStatus(QStringLiteral("Voice authorization failed or expired"));
            emit runningChanged();
            return;
        }
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(response, &parse);
        response.fill(0);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
            else setStatus(QStringLiteral("Invalid voice authorization response"));
            emit runningChanged();
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
            if (recheck) stopWithReason(QStringLiteral("Voice access recheck failed; join again manually"));
            else setStatus(QStringLiteral("Rejected unsafe voice room credentials"));
            emit runningChanged();
            return;
        }
        if (recheck) {
            // Revalidate membership/session only. This JWT is not an SDK token
            // renewal and is never used to silently rejoin or enable a mic.
            authorizationTimer_.start(qMax(100, runtime_.authorizationIntervalMs));
        } else {
            launch(publishMicrophone, server, token);
        }
        emit runningChanged();
    });
}

void VoiceSession::launch(bool publishMicrophone, const QString &url, const QString &token) {
    if (running()) return;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LIVEKIT_URL"), url);
    env.insert(QStringLiteral("LIVEKIT_TOKEN"), token);
    env.remove(QStringLiteral("GCP_ACCESS_TOKEN"));
    env.remove(QStringLiteral("GCP_REFRESH_TOKEN"));
    stopReason_.clear();
    muted_ = true;
    transmitting_ = false;
    emit controlsChanged();
    stopRequested_ = false;
    connectionTimedOut_ = false;
    deviceLost_ = false;
    connected_ = false;
    outputBuffer_.clear();
    microphoneEnabled_ = publishMicrophone;
    process_.setProcessEnvironment(env);
    process_.setProgram(nativeProgram());
    QStringList args{QStringLiteral("join"), QStringLiteral("--desktop-control"),
                     QStringLiteral("--seconds"), QStringLiteral("0")};
    if (publishMicrophone && pushToTalk_) args << QStringLiteral("--push-to-talk");
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
    ++generation_;
    heartbeatTimer_.stop();
    authorizationTimer_.stop();
    authorizationWatchdog_.stop();
    muteWatchdog_.stop();
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
    // Native stop mutes and disconnects orderly; watchdog handles a stuck SDK.
    if (!sendControl("stop\n")) process_.kill();
    process_.closeWriteChannel();
    stopWatchdog_.start(2000);
}

void VoiceSession::resetControls() {
    heartbeatTimer_.stop();
    authorizationTimer_.stop();
    authorizationWatchdog_.stop();
    muteWatchdog_.stop();
    if (pending_) {
        auto *reply = pending_;
        pending_ = nullptr;
        reply->abort();
    }
    muted_ = true;
    transmitting_ = false;
    emit controlsChanged();
}

bool VoiceSession::sendControl(const QByteArray &command) {
    if (process_.state() != QProcess::Running || process_.bytesToWrite() > 1024) return false;
    return process_.write(command) == command.size();
}

void VoiceSession::stopWithReason(const QString &reason) {
    stopReason_ = reason;
    leave();
    setStatus(reason);
}

void VoiceSession::setMuted(bool muted) {
    if (!microphoneEnabled_ || !connected_ || stopRequested_) return;
    if (!sendControl(muted ? "mute\n" : "unmute\n")) {
        stopWithReason(QStringLiteral("Voice control failed; stopped for microphone safety"));
        return;
    }
    muted_ = muted;
    if (muted && transmitting_) muteWatchdog_.start(1000);
    emit controlsChanged();
}

void VoiceSession::setPushToTalk(bool enabled) {
    if (pushToTalk_ == enabled) return;
    if (running()) {
        if (!connected_ || stopRequested_) return;
        if (!sendControl(enabled ? "mode_ptt\n" : "mode_open\n")) {
            stopWithReason(QStringLiteral("Voice mode control failed; stopping voice"));
            return;
        }
        if (transmitting_) muteWatchdog_.start(1000);
    }
    pushToTalk_ = enabled;
    muted_ = true; // Explicit Enable mic is required after any mode switch.
    emit controlsChanged();
}
