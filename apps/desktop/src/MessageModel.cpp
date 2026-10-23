#include "MessageModel.h"
#include <algorithm>
#include <utility>

int MessageModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
QVariant MessageModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const auto &row = rows_[index.row()];
    switch (role) {
    case Id: return row.id;
    case AuthorId: return row.author;
    case AuthorName: return names_.value(row.author, QStringLiteral("Member ") + row.author.left(8));
    case Body: return row.deletedMs ? QStringLiteral("[deleted]") : row.body;
    case Created: return row.createdUs / 1000;
    case Edited: return row.editedMs != 0;
    case Deleted: return row.deletedMs != 0;
    default: return {};
    }
}
QHash<int, QByteArray> MessageModel::roleNames() const {
    return {{Id, "messageId"}, {AuthorId, "authorId"}, {AuthorName, "authorName"},
        {Body, "bodyText"}, {Created, "createdAtMs"}, {Edited, "edited"}, {Deleted, "deleted"}};
}
bool MessageModel::before(const MessageRow &a, const MessageRow &b) {
    return a.createdUs == b.createdUs ? a.id < b.id : a.createdUs < b.createdUs;
}
std::optional<MessageRow> MessageModel::find(const QString &id) const {
    const auto found = std::find_if(rows_.cbegin(), rows_.cend(), [&](const auto &row) { return row.id == id; });
    return found == rows_.cend() ? std::nullopt : std::optional<MessageRow>(*found);
}
void MessageModel::replace(QVector<MessageRow> rows) {
    std::sort(rows.begin(), rows.end(), before);
    if (rows.size() > maximumRows) rows.remove(0, rows.size() - maximumRows);
    beginResetModel(); rows_ = std::move(rows); endResetModel();
}
void MessageModel::upsert(const MessageRow &row, bool insert, bool keepNewest) {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id != row.id) continue;
        // A delayed create/GET acknowledgment cannot resurrect a tombstone or
        // overwrite an edit that is known to be newer.
        if (rows_[i].createdUs != row.createdUs || rows_[i].author != row.author ||
            rows_[i].deletedMs || rows_[i].editedMs > row.editedMs) return;
        rows_[i] = row;
        emit dataChanged(index(i), index(i));
        return;
    }
    if (!insert) return;
    const auto at = static_cast<int>(std::lower_bound(rows_.cbegin(), rows_.cend(), row, before) - rows_.cbegin());
    beginInsertRows({}, at, at); rows_.insert(at, row); endInsertRows();
    if (rows_.size() > maximumRows) {
        const int drop = keepNewest ? 0 : static_cast<int>(rows_.size()) - 1;
        beginRemoveRows({}, drop, drop); rows_.removeAt(drop); endRemoveRows();
    }
}
void MessageModel::authors(const QHash<QString, QString> &names) {
    if (names_ == names) return;
    names_ = names;
    if (!rows_.isEmpty()) emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {AuthorName});
}
