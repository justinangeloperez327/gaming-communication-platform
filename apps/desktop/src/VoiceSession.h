#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
class AccountSession;
class ChannelDirectory;
#include <QProcess>
#include <QString>
#include <QTimer>

class VoiceSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY runningChanged)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
public:
    explicit VoiceSession(AccountSession *account, ChannelDirectory *directory, QObject *parent = nullptr);
    bool connected() const { return connected_; }
    bool running() const { return process_.state() != QProcess::NotRunning; }
    bool busy() const { return pending_ != nullptr || running(); }
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
    void launch(bool publishMicrophone, const QString &url, const QString &token, int lifetime);
    void setStatus(const QString &value);
    AccountSession *account_ = nullptr;
    ChannelDirectory *directory_ = nullptr;
    QProcess process_;
    QNetworkAccessManager network_;
    QNetworkReply *pending_ = nullptr;
    QTimer startupWatchdog_;
    QTimer stopWatchdog_;
    QTimer connectWatchdog_;
    bool stopRequested_ = false;
    bool connectionTimedOut_ = false;
    bool connected_ = false;
    QByteArray outputBuffer_;
    bool microphoneEnabled_ = false;
    QString status_ = QStringLiteral("Disconnected");
};
