#pragma once
#include <QObject>
#include <QProcess>
#include <QString>

class VoiceSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
public:
    explicit VoiceSession(QObject *parent = nullptr);
    bool running() const { return process_.state() != QProcess::NotRunning; }
    bool microphoneEnabled() const { return microphoneEnabled_; }
    QString status() const { return status_; }
    bool available() const;
    Q_INVOKABLE void joinListenOnly();
    Q_INVOKABLE void joinWithMicrophone(bool explicitConsent);
    Q_INVOKABLE void leave();
    Q_INVOKABLE void refreshAvailability() { emit availabilityChanged(); }
signals:
    void runningChanged();
    void statusChanged();
    void availabilityChanged();
private:
    void start(bool publishMicrophone);
    void setStatus(const QString &value);
    QProcess process_;
    bool microphoneEnabled_ = false;
    QString status_ = QStringLiteral("Disconnected");
};
