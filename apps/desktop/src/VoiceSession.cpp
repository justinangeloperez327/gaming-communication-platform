#include "VoiceSession.h"

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

VoiceSession::VoiceSession(QObject *parent) : QObject(parent) {
    connect(&process_, &QProcess::started, this, [this]() {
        setStatus(microphoneEnabled_ ? QStringLiteral("Native voice process started · microphone enabled")
                                    : QStringLiteral("Native voice process started · listen only"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        microphoneEnabled_ = false;
        setStatus(exitStatus == QProcess::NormalExit && exitCode == 0
                      ? QStringLiteral("Disconnected")
                      : QStringLiteral("Voice process stopped; connection not verified"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            microphoneEnabled_ = false;
            setStatus(QStringLiteral("Cannot start native voice executable"));
            emit runningChanged();
        }
    });
    // Never log child output or backend tokens.
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() { process_.readAllStandardOutput(); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this]() { process_.readAllStandardError(); });
}

bool VoiceSession::available() const {
#ifdef Q_OS_WIN
    return QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
#else
    return false;
#endif
}

void VoiceSession::setStatus(const QString &value) {
    if (status_ != value) { status_ = value; emit statusChanged(); }
}

void VoiceSession::start(bool publishMicrophone) {
    if (busy()) return;
    if (!available()) { setStatus(QStringLiteral("Native voice executable is missing")); return; }
    const auto env = QProcessEnvironment::systemEnvironment();
    const QUrl api(env.value(QStringLiteral("GCP_API_URL")));
    const auto access = env.value(QStringLiteral("GCP_ACCESS_TOKEN"));
    const auto community = env.value(QStringLiteral("GCP_COMMUNITY_ID"));
    const auto channel = env.value(QStringLiteral("GCP_VOICE_CHANNEL_ID"));
    if (!permittedUrl(api) || access.size() != 64 || !uuidValid(community) || !uuidValid(channel)) {
        setStatus(QStringLiteral("Authenticated backend voice configuration unavailable"));
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
        setStatus(QStringLiteral("GCP_API_URL must not contain a path"));
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
    microphoneEnabled_ = publishMicrophone;
    process_.setProcessEnvironment(env);
    process_.setProgram(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
    QStringList args{QStringLiteral("join"), QStringLiteral("--seconds"), QString::number(qMin(lifetime, 300))};
    if (publishMicrophone) {
        args << QStringLiteral("--publish-mic") << QStringLiteral("--confirm-microphone") << QStringLiteral("yes");
    }
    process_.setArguments(args);
    setStatus(QStringLiteral("Starting native voice…"));
    process_.start();
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
    setStatus(QStringLiteral("Disconnecting…"));
    process_.terminate();
    QTimer::singleShot(2000, this, [this]() { if (running()) process_.kill(); });
}
