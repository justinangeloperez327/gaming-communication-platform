#pragma once
#include "MessageModel.h"
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
#include <memory>

class AccountSession;
class ChannelDirectory;
struct ChatRuntime { int retryBaseMs = 1000; int pollMs = 30000; int deadlineMs = 10000; };

class ChatSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *messages READ messages CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool live READ live NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool sending READ sending NOTIFY changed)
    Q_PROPERTY(bool canSubmit READ canSubmit NOTIFY changed)
    Q_PROPERTY(bool canMutate READ canMutate NOTIFY changed)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY changed)
    Q_PROPERTY(bool browsingOlder READ browsingOlder NOTIFY changed)
    Q_PROPERTY(bool newerAvailable READ newerAvailable NOTIFY changed)
    Q_PROPERTY(QString draft READ draft NOTIFY draftChanged)
    Q_PROPERTY(int draftBytes READ draftBytes NOTIFY draftChanged)
    Q_PROPERTY(bool retrySend READ retrySend NOTIFY draftChanged)
public:
    explicit ChatSession(AccountSession *account, ChannelDirectory *directory, QObject *parent = nullptr, ChatRuntime runtime = {});
    ~ChatSession() override;
    QAbstractItemModel *messages() { return &model_; }
    QString status() const { return status_; }
    bool live() const { return socketReady_; }
    bool loading() const { return read_ != nullptr; }
    bool sending() const { return mutation_ != nullptr; }
    bool canMutate() const;
    bool canSubmit() const;
    bool hasOlder() const { return hasOlder_; }
    bool browsingOlder() const { return !followingLatest_; }
    bool newerAvailable() const { return newerAvailable_; }
    QString draft() const;
    int draftBytes() const { return static_cast<int>(draft().toUtf8().size()); }
    bool retrySend() const;
    Q_INVOKABLE bool setDraft(const QString &body);
    Q_INVOKABLE void clearDraft();
    Q_INVOKABLE void sendDraft();
    Q_INVOKABLE void editMessage(const QString &id, const QString &body);
    Q_INVOKABLE void deleteMessage(const QString &id);
    Q_INVOKABLE void refreshLatest();
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE int utf8Bytes(const QString &text) const { return static_cast<int>(text.toUtf8().size()); }
signals:
    void changed();
    void draftChanged();
    void historyReset();
    void accessRevoked();
    void mutationFinished(const QString &id, bool success);
private:
    enum class Read { Members, Latest, Older, Message };
    enum class Mutation { Create, Edit, Delete };
    struct Draft { QString body, attempt; };
    void selection();
    void accountChanged();
    void stopTransport();
    void stopSocket();
    void abortReply(QNetworkReply *&reply);
    void openSocket();
    void socketEvent(const QString &text);
    void socketFailed();
    void queueMessage(const QString &id);
    void pump();
    void read(Read operation, const QString &id = {});
    void mutate(Mutation operation, const QString &id, const QString &body = {});
    bool applyPage(const QByteArray &bytes, bool older, const QString &cursor);
    bool applyMembers(const QByteArray &bytes);
    std::optional<MessageRow> parseMessage(const QJsonObject &object) const;
    void merge(const MessageRow &row);
    void resolveAttempt(const MessageRow &row);
    void accessLost();
    void setStatus(const QString &status);
    QString path() const;
    QString draftKey() const;
    AccountSession *account_;
    ChannelDirectory *directory_;
    ChatRuntime runtime_;
    MessageModel model_;
    QNetworkAccessManager network_;
    QNetworkReply *read_ = nullptr, *mutation_ = nullptr;
    std::unique_ptr<QWebSocket> socket_;
    QTimer pumpTimer_, retryTimer_, socketDeadline_, pollTimer_, accountDeadline_, heartbeatTimer_;
    QString community_, channel_, token_, before_, status_ = QStringLiteral("Choose a text channel");
    QHash<QString, Draft> drafts_;
    QHash<QString, QString> names_;
    QSet<QString> events_;
    quint64 generation_ = 0;
    int failures_ = 0;
    bool socketReady_ = false, authorized_ = false, blocked_ = false;
    bool wantLatest_ = false, wantMembers_ = false, wantOlder_ = false;
    bool hasOlder_ = false, followingLatest_ = true, newerAvailable_ = false, awaitingAccount_ = false;
};
