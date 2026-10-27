#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

// One explicit, unauthenticated readiness request. Never retries or touches media.
class ServerProbe final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString result READ result NOTIFY changed)
public:
    explicit ServerProbe(QObject* parent = nullptr, int timeoutMs = 5000);
    ~ServerProbe() override;
    bool busy() const { return pending_ != nullptr; }
    QString status() const { return status_; }
    QString result() const { return result_; }
    Q_INVOKABLE void check(const QString& origin);
    Q_INVOKABLE void clear();
signals:
    void changed();
private:
    QNetworkAccessManager network_;
    QNetworkReply* pending_ = nullptr;
    const int timeoutMs_;
    QString status_, result_ = QStringLiteral("idle");
};
