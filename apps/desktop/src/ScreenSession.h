#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTimer>
#include <QUrl>
class AccountSession;
class ChannelDirectory;
struct ScreenRuntime {
    QString program;
    int renewMs = 20000;
    int requestMs = 10000;
    int startMs = 20000;
};
// Qt owns account authorization and explicit consent; the child retains the
// exact OS picker item and enforces the same absolute lease deadline.
class ScreenSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool sharing READ sharing NOTIFY changed)
    Q_PROPERTY(bool awaitingConsent READ awaitingConsent NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(int sourceWidth READ sourceWidth NOTIFY changed)
    Q_PROPERTY(int sourceHeight READ sourceHeight NOTIFY changed)
    Q_PROPERTY(QString scopeLabel READ scopeLabel NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit ScreenSession(AccountSession* account, ChannelDirectory* directory,
                           QObject* parent = nullptr, ScreenRuntime runtime = {});
    ~ScreenSession() override;
    bool available() const;
    bool busy() const;
    bool sharing() const { return sharing_; }
    bool awaitingConsent() const { return awaitingConsent_; }
    QString sourceName() const { return sourceName_; }
    int sourceWidth() const { return width_; }
    int sourceHeight() const { return height_; }
    QString scopeLabel() const { return scopeLabel_; }
    QString status() const { return status_; }
    Q_INVOKABLE void chooseSource();
    Q_INVOKABLE void confirmShare(bool consent);
    Q_INVOKABLE void stop();
signals:
    void changed();
    void consentRequested();
    void consentInvalidated();
private:
    QString program() const;
    QString path() const;
    bool sameScope() const;
    void contextChanged();
    void requestGrant(bool renew);
    bool applyGrant(const QByteArray& bytes, bool renew, qint64 requestedAt);
    bool control(const QByteArray& line);
    void readOutput();
    void stopWithReason(const QString& reason);
    void revoke();
    void abortPending();
    void processFinished();
    AccountSession* account_;
    ChannelDirectory* directory_;
    ScreenRuntime runtime_;
    QNetworkAccessManager network_;
    QNetworkReply* pending_ = nullptr;
    QNetworkReply* cleanup_ = nullptr;
    QProcess process_;
    QTimer heartbeat_, renewal_, deadline_, startup_, stopWatchdog_;
    QUrl origin_;
    QString user_, community_, channel_, scopeLabel_, access_, lease_;
    QString sourceName_, status_ = QStringLiteral("Screen sharing stopped"), stopReason_;
    QByteArray output_;
    int width_ = 0, height_ = 0;
    bool awaitingConsent_ = false, consent_ = false, sharing_ = false;
    bool stopping_ = false, connected_ = false, capturing_ = false;
    qint64 leaseDeadline_ = 0;
    quint64 generation_ = 0;
};
