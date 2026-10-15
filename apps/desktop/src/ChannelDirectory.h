#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVariantList>
class AccountSession;
class ChannelDirectory final : public QObject {
 Q_OBJECT
 Q_PROPERTY(QVariantList communities READ communities NOTIFY changed)
 Q_PROPERTY(QVariantList channels READ channels NOTIFY changed)
 Q_PROPERTY(QString communityId READ communityId NOTIFY changed)
 Q_PROPERTY(QString channelId READ channelId NOTIFY changed)
 Q_PROPERTY(QString status READ status NOTIFY changed)
 Q_PROPERTY(bool busy READ busy NOTIFY changed)
public:
 explicit ChannelDirectory(AccountSession *account, QObject *parent=nullptr);
 QVariantList communities() const { return communities_; }
 QVariantList channels() const { return channels_; }
 QString communityId() const { return communityId_; }
 QString channelId() const { return channelId_; }
 QString status() const { return status_; }
 bool busy() const { return pending_ != nullptr; }
 Q_INVOKABLE void refresh();
 Q_INVOKABLE void chooseCommunity(const QString &id);
 Q_INVOKABLE void chooseVoiceChannel(const QString &id);
 Q_INVOKABLE void reset();
signals:
 void changed();
 void selectionChanged();
private:
 void request(const QString &path, bool listCommunities);
 AccountSession *account_;
 QNetworkAccessManager network_;
 QNetworkReply *pending_ = nullptr;
 QVariantList communities_, channels_;
 QString communityId_, channelId_, status_ = QStringLiteral("Sign in to view communities");
 quint64 version_ = 0;
};
