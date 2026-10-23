#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
class AccountSession;
struct FriendsRuntime { int pollMs = 30000; int deadlineMs = 10000; };

// One bounded operation at a time. A refresh publishes all four lists together.
class FriendsDirectory final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList friends READ friends NOTIFY changed)
    Q_PROPERTY(QVariantList requests READ requests NOTIFY changed)
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY changed)
    Q_PROPERTY(QVariantList conversations READ conversations NOTIFY changed)
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY changed)
    Q_PROPERTY(QString peerId READ peerId NOTIFY changed)
    Q_PROPERTY(QString peerName READ peerName NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool mutating READ mutating NOTIFY changed)
    Q_PROPERTY(bool verified READ verified NOTIFY changed)
public:
    explicit FriendsDirectory(AccountSession *account, QObject *parent = nullptr, FriendsRuntime runtime = {});
    ~FriendsDirectory() override;
    QVariantList friends() const { return friends_; }
    QVariantList requests() const { return requests_; }
    QVariantList blocks() const { return blocks_; }
    QVariantList conversations() const { return conversations_; }
    QString conversationId() const { return conversation_; }
    QString peerId() const { return peer_; }
    QString peerName() const;
    QString status() const { return status_; }
    bool busy() const { return pending_ != nullptr || processing_; }
    bool mutating() const { return mutation_; }
    bool verified() const { return verified_; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void sendRequest(const QString &handle);
    Q_INVOKABLE void acceptRequest(const QString &id);
    Q_INVOKABLE void declineRequest(const QString &id);
    Q_INVOKABLE void cancelRequest(const QString &id);
    Q_INVOKABLE void removeFriend(const QString &id);
    Q_INVOKABLE void blockUser(const QString &id);
    Q_INVOKABLE void unblockUser(const QString &id);
    Q_INVOKABLE void openConversation(const QString &userId);
    Q_INVOKABLE void chooseConversation(const QString &id);
    Q_INVOKABLE void closeConversation();
    Q_INVOKABLE void cancel();
    void invalidateConversation(const QString &id);
signals:
    void changed();
    void selectionChanged();
    void relationshipLost(const QString &peer);
private:
    enum class Operation { Friends, Requests, Blocks, Conversations, Send, Accept, Decline, Cancel, Remove, Block, Unblock, Open };
    void accountChanged();
    void reset();
    bool ready() const;
    void abort();
    void send(Operation operation, const QByteArray &method, const QString &path, const QJsonObject &body = {}, const QString &target = {});
    bool apply(Operation operation, const QByteArray &bytes, const QString &target);
    void failed(int http, QNetworkReply::NetworkError error);
    void publish();
    void revokePeer(const QString &peer);
    void select(const QVariantMap &conversation);
    void resolveRequest(const QString &id, Operation operation);
    AccountSession *account_;
    FriendsRuntime runtime_;
    QNetworkAccessManager network_;
    QNetworkReply *pending_ = nullptr;
    QTimer poll_;
    QVariantList friends_, requests_, blocks_, conversations_;
    QVariantList nextFriends_, nextRequests_, nextBlocks_, nextConversations_;
    QString conversation_, peer_, token_, user_, outcome_, status_ = QStringLiteral("Sign in to manage friends");
    quint64 version_ = 0;
    bool processing_ = false, mutation_ = false, verified_ = false, refreshWanted_ = false;
};
