#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVariantList>
#include <QVariantMap>

class AccountSession;

// One bounded request at a time; account must outlive the directory.
class ChannelDirectory final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList communities READ communities NOTIFY changed)
    Q_PROPERTY(QVariantList channels READ channels NOTIFY changed)
    Q_PROPERTY(QVariantList allChannels READ allChannels NOTIFY changed)
    Q_PROPERTY(QVariantList members READ members NOTIFY changed)
    Q_PROPERTY(QVariantList invitations READ invitations NOTIFY changed)
    Q_PROPERTY(QString communityId READ communityId NOTIFY changed)
    Q_PROPERTY(QString communityName READ communityName NOTIFY changed)
    Q_PROPERTY(QString communityRole READ communityRole NOTIFY changed)
    Q_PROPERTY(QString channelId READ channelId NOTIFY changed)
    Q_PROPERTY(QString channelName READ channelName NOTIFY changed)
    Q_PROPERTY(QString invitationToken READ invitationToken NOTIFY changed)
    Q_PROPERTY(QString invitationId READ invitationId NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canManage READ canManage NOTIFY changed)
    Q_PROPERTY(bool managementLoaded READ managementLoaded NOTIFY changed)
public:
    explicit ChannelDirectory(AccountSession *account, QObject *parent = nullptr);
    ~ChannelDirectory() override;
    QVariantList communities() const { return communities_; }
    QVariantList channels() const { return channels_; } // Voice picker only.
    QVariantList allChannels() const { return allChannels_; }
    QVariantList members() const { return members_; }
    QVariantList invitations() const { return invitations_; }
    QString communityId() const { return communityId_; }
    QString communityName() const { return communityName_; }
    QString communityRole() const { return communityRole_; }
    QString channelId() const { return channelId_; }
    QString channelName() const;
    QString invitationToken() const { return invitationToken_; }
    QString invitationId() const { return invitationId_; }
    QString status() const { return status_; }
    bool busy() const { return pending_ != nullptr || processing_; }
    bool canManage() const;
    bool managementLoaded() const { return managementLoaded_; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void chooseCommunity(const QString &id);
    Q_INVOKABLE void chooseVoiceChannel(const QString &id);
    Q_INVOKABLE void loadManagement();
    Q_INVOKABLE void createCommunity(const QString &name);
    Q_INVOKABLE void joinCommunity(const QString &token);
    Q_INVOKABLE void renameCommunity(const QString &name);
    Q_INVOKABLE void deleteCommunity();
    Q_INVOKABLE void leaveCommunity();
    Q_INVOKABLE void createChannel(const QString &name, const QString &kind, int position);
    Q_INVOKABLE void updateChannel(const QString &id, const QString &name, int position);
    Q_INVOKABLE void deleteChannel(const QString &id);
    Q_INVOKABLE void createInvitation();
    Q_INVOKABLE void revokeInvitation(const QString &id);
    Q_INVOKABLE void setMemberRole(const QString &id, const QString &role);
    Q_INVOKABLE void removeMember(const QString &id);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
signals:
    void changed();
    void selectionChanged();
private:
    enum class Operation {
        ListCommunities, GetCommunity, ListChannels, ListMembers, ListInvitations,
        CreateCommunity, JoinCommunity, RenameCommunity, DeleteCommunity, LeaveCommunity,
        CreateChannel, UpdateChannel, DeleteChannel, CreateInvitation, RevokeInvitation,
        SetMemberRole, RemoveMember
    };
    bool ready() const;
    bool validName(const QString &name, int limit);
    bool validChannel(const QString &name, const QString &kind, int position);
    void send(Operation operation, const QByteArray &method, const QString &path,
              const QByteArray &payload = {}, const QString &target = {});
    bool applyResponse(Operation operation, const QByteArray &bytes, const QString &target);
    void failed(Operation operation, int http, QNetworkReply::NetworkError error);
    void reloadSelected(bool management);
    void finish();
    void useCommunity(const QVariantMap &community);
    void clearSelection();
    void clearInvitations();
    void upsertCommunity(const QVariantMap &community);
    QString selectedPath() const;
    AccountSession *account_;
    QNetworkAccessManager network_;
    QNetworkReply *pending_ = nullptr;
    QVariantList communities_, channels_, allChannels_, members_, invitations_;
    QString communityId_, communityName_, communityRole_, channelId_;
    QString invitationToken_, invitationId_, outcome_;
    QString status_ = QStringLiteral("Sign in to view communities");
    quint64 version_ = 0;
    bool processing_ = false;
    bool hadSession_ = false;
    bool managementLoaded_ = false;
    bool detailsWanted_ = false;
};
