#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QString>
#include <QUrl>
#include <QVariantList>

class AccountSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString userId READ userId NOTIFY changed)
    Q_PROPERTY(bool serverOwner READ serverOwner NOTIFY changed)
    Q_PROPERTY(bool invitationBusy READ invitationBusy NOTIFY changed)
    Q_PROPERTY(QString invitationStatus READ invitationStatus NOTIFY changed)
    Q_PROPERTY(QString invitationCode READ invitationCode NOTIFY changed)
    Q_PROPERTY(QString invitationEmail READ invitationEmail NOTIFY changed)
    Q_PROPERTY(QVariantList invitations READ invitations NOTIFY changed)
public:
    explicit AccountSession(QObject *parent = nullptr);
    ~AccountSession() override;
    bool signedIn() const { return !access_.isEmpty(); }
    bool busy() const { return pending_ != nullptr; }
    QString status() const { return status_; }
    QString displayName() const { return displayName_; }
    QString userId() const { return userId_; }
    bool serverOwner() const { return serverOwner_; }
    bool invitationBusy() const { return invitationPending_ != nullptr; }
    QString invitationStatus() const { return invitationStatus_; }
    QString invitationCode() const { return invitationCode_; }
    QString invitationEmail() const { return invitationEmail_; }
    QVariantList invitations() const { return invitations_; }
    QString accessToken() const { return access_; }
    QUrl apiOrigin() const { return origin_; }
    Q_INVOKABLE void signIn(const QString &origin, const QString &email, const QString &password);
    Q_INVOKABLE void createAccount(const QString &origin, const QString &handle,
        const QString &displayName, const QString &email, const QString &password,
        const QString &invitation);
    Q_INVOKABLE void inviteFriend(const QString &email);
    Q_INVOKABLE void revokeInvitation();
    Q_INVOKABLE void revokePendingInvitation(const QString &id);
    Q_INVOKABLE void loadInvitations();
    Q_INVOKABLE void signOut();
signals:
    void changed();
    void signedOut();
private:
    enum class AuthOperation { SignIn, Register, Refresh };
    enum class InvitationOperation { Create, List, Revoke };
    void clearSession();
    void refresh();
    void sendAuth(const QString &path, const QByteArray &payload, AuthOperation operation);
    void sendInvitation(InvitationOperation operation, const QByteArray &payload = {}, const QString &targetId = {});
    bool applyResponse(const QByteArray &bytes, bool isRefresh);
    QNetworkAccessManager network_;
    QNetworkReply *pending_ = nullptr;
    QNetworkReply *invitationPending_ = nullptr;
    QTimer refreshTimer_;
    QUrl origin_;
    QString access_;
    QString refresh_;
    QString status_ = QStringLiteral("Signed out");
    QString displayName_;
    QString userId_;
    bool serverOwner_ = false;
    QString invitationStatus_;
    QString invitationCode_;
    QString invitationId_;
    QString invitationEmail_;
    QVariantList invitations_;
    quint64 generation_ = 0;
};
