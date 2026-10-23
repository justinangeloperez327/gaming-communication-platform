#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>
#include <optional>

struct MessageRow {
    QString id, channel, author, body;
    qint64 createdUs = 0, editedMs = 0, deletedMs = 0;
};

class MessageModel final : public QAbstractListModel {
    Q_OBJECT
public:
    static constexpr int maximumRows = 200;
    enum Role { Id = Qt::UserRole + 1, AuthorId, AuthorName, Body, Created, Edited, Deleted };
    explicit MessageModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    std::optional<MessageRow> find(const QString &id) const;
    QVector<MessageRow> rows() const { return rows_; }
    void replace(QVector<MessageRow> rows);
    void upsert(const MessageRow &row, bool insert, bool keepNewest);
    void authors(const QHash<QString, QString> &names);
    static bool before(const MessageRow &a, const MessageRow &b);
private:
    QVector<MessageRow> rows_;
    QHash<QString, QString> names_;
};
