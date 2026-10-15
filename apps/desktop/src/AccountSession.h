#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QString>
#include <QUrl>

class AccountSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
public:
    explicit AccountSession(QObject *parent = nullptr);
    bool signedIn() const { return !access_.isEmpty(); }
    bool busy() const { return pending_ != nullptr; }
    QString status() const { return status_; }
    QString displayName() const { return displayName_; }
    QString accessToken() const { return access_; }
    QUrl apiOrigin() const { return origin_; }
    Q_INVOKABLE void signIn(const QString &origin, const QString &email, const QString &password);
    Q_INVOKABLE void signOut();
signals:
    void changed();
    void signedOut();
private:
    void clearSession();
    void refresh();
    void sendAuth(const QString &path, const QByteArray &payload, bool isRefresh);
    bool applyResponse(const QByteArray &bytes, bool isRefresh);
    QNetworkAccessManager network_;
    QNetworkReply *pending_ = nullptr;
    QTimer refreshTimer_;
    QUrl origin_;
    QString access_;
    QString refresh_;
    QString status_ = QStringLiteral("Signed out");
    QString displayName_;
    quint64 generation_ = 0;
};
