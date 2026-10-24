#pragma once
#include <QObject>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include "../../../native/transport/src/view_control.h"
class AccountSession;
class ChannelDirectory;
struct ViewerRuntime {
    QString program;
    int renewMs = 20000, requestMs = 10000, startMs = 20000, frameMs = 33;
};
class ScreenViewer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool watching READ watching NOTIFY changed)
    Q_PROPERTY(QVariantList publishers READ publishers NOTIFY changed)
    Q_PROPERTY(QString publisherId READ publisherId NOTIFY changed)
    Q_PROPERTY(QString publisherName READ publisherName NOTIFY changed)
    Q_PROPERTY(QString scopeLabel READ scopeLabel NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit ScreenViewer(AccountSession* account, ChannelDirectory* directory,
                          QObject* parent = nullptr, ViewerRuntime runtime = {});
    ~ScreenViewer() override;
    bool available() const;
    bool busy() const;
    bool watching() const { return watching_; }
    QVariantList publishers() const { return publishers_; }
    QString publisherId() const { return selected_; }
    QString publisherName() const { return publisherName_; }
    QString scopeLabel() const { return scopeLabel_; }
    QString status() const { return status_; }
    QImage image() const { return image_; }
    int rotation() const { return rotation_; }
    quint64 frameNumber() const { return frameNumber_; }
    Q_INVOKABLE void refreshPublishers();
    Q_INVOKABLE void choosePublisher(const QString& id);
    Q_INVOKABLE void watch(bool consent);
    Q_INVOKABLE void stop();
signals:
    void changed();
    void frameChanged();
    void started();
    void ended();
private:
    QString program() const;
    QString path() const;
    bool sameScope() const;
    void contextChanged();
    void requestGrant(bool renew);
    void requestTrack();
    bool applyGrant(const QByteArray&, bool renew, qint64 requestedAt);
    bool control(const QByteArray&);
    void readOutput();
    void requestFrame();
    void stopWithReason(const QString& reason);
    void revoke();
    void abort(QNetworkReply*&);
    void processFinished();
    AccountSession* account_;
    ChannelDirectory* directory_;
    ViewerRuntime runtime_;
    QNetworkAccessManager network_;
    QNetworkReply* pending_ = nullptr;
    QNetworkReply* listing_ = nullptr;
    QNetworkReply* cleanup_ = nullptr;
    QProcess process_;
    QTimer heartbeat_, renewal_, deadline_, startup_, kill_, frameTimer_, payloadDeadline_;
    QUrl origin_;
    QString user_, community_, channel_, scopeLabel_, access_, lease_, selected_, publisherName_, trackSid_;
    QVariantList publishers_;
    QString status_ = QStringLiteral("Choose a voice channel, then refresh shares"), stopReason_;
    QByteArray line_, pixels_;
    std::optional<gcp::transport::ViewFrameHeader> header_;
    QImage image_;
    quint64 generation_ = 0, frameNumber_ = 0;
    qint64 leaseDeadline_ = 0;
    int rotation_ = 0;
    bool consent_ = false, stopping_ = true, connected_ = false, subscribed_ = false;
    bool watching_ = false, credit_ = false;
};
