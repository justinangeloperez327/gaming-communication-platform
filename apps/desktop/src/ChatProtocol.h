#pragma once
#include <QString>
#include <QUrl>
#include <QUuid>
namespace ChatProtocol {
inline bool uuid(const QString &value) {
    const QUuid id(value);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == value;
}
inline bool bodyValid(const QString &body) {
    if (body.trimmed().isEmpty() || body.toUtf8().size() > 4000) return false;
    for (const auto c : body)
        if (c.category() == QChar::Other_Control && c != QChar('\n') && c != QChar('\t')) return false;
    return true;
}
inline bool originValid(const QUrl &url) {
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
        (url.path().isEmpty() || url.path() == QStringLiteral("/")) &&
        (url.scheme() == QStringLiteral("https") || (url.scheme() == QStringLiteral("http") &&
            (url.host() == QStringLiteral("localhost") || url.host() == QStringLiteral("127.0.0.1"))));
}
}
