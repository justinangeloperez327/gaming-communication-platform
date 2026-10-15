#include "VoiceSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTimer>

VoiceSession::VoiceSession(QObject *parent) : QObject(parent) {
    connect(&process_, &QProcess::started, this, [this]() {
        setStatus(microphoneEnabled_ ? QStringLiteral("Voice process running · microphone authorized")
                                    : QStringLiteral("Voice process running · listen only"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        microphoneEnabled_ = false;
        setStatus(exitStatus == QProcess::NormalExit && exitCode == 0
                      ? QStringLiteral("Disconnected")
                      : QStringLiteral("Voice process stopped (check connection and device settings)"));
        emit runningChanged();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            microphoneEnabled_ = false;
            setStatus(QStringLiteral("Cannot start native voice executable"));
            emit runningChanged();
        }
    });
    // Credentials, stdout and stderr must never be copied into UI text or log output.
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        process_.readAllStandardOutput();
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this]() {
        process_.readAllStandardError();
    });
}

bool VoiceSession::available() const {
#ifdef Q_OS_WIN
    return QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
#else
    return false;
#endif
}

void VoiceSession::setStatus(const QString &value) {
    if (status_ != value) {
        status_ = value;
        emit statusChanged();
    }
}

void VoiceSession::start(bool publishMicrophone) {
    if (running()) { return; }
    if (!available()) {
        setStatus(QStringLiteral("Native voice executable is not installed beside desktop app"));
        return;
    }
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto url = env.value(QStringLiteral("LIVEKIT_URL"));
    const auto token = env.value(QStringLiteral("LIVEKIT_TOKEN"));
    if (url.isEmpty() || token.isEmpty()) {
        setStatus(QStringLiteral("Voice credentials unavailable (LIVEKIT_URL and LIVEKIT_TOKEN required)"));
        return;
    }
    // The native executable independently validates URL security and token inputs.
    microphoneEnabled_ = publishMicrophone;
    process_.setProcessEnvironment(env);
    process_.setProgram(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gcp-livekit-voice.exe")));
    QStringList args{QStringLiteral("join"), QStringLiteral("--seconds"), QStringLiteral("300")};
    if (publishMicrophone) {
        args << QStringLiteral("--publish-mic") << QStringLiteral("--confirm-microphone") << QStringLiteral("yes");
    }
    process_.setArguments(args);
    setStatus(QStringLiteral("Connecting…"));
    process_.start();
    emit runningChanged();
}

void VoiceSession::joinListenOnly() { start(false); }
void VoiceSession::joinWithMicrophone(bool explicitConsent) {
    if (!explicitConsent) {
        setStatus(QStringLiteral("Microphone permission must be explicitly confirmed"));
        return;
    }
    start(true);
}
void VoiceSession::leave() {
    if (!running()) { return; }
    setStatus(QStringLiteral("Disconnecting…"));
    process_.terminate();
    QTimer::singleShot(2000, this, [this]() {
        if (running()) { process_.kill(); }
    });
}
