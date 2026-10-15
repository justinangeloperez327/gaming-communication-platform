#include "ChannelDirectory.h"
#include "AccountSession.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUuid>
#include <QUrl>
namespace {
 bool validId(const QString &id) { return !QUuid::fromString(id).isNull(); }
 QVariantList rows(const QJsonArray &array, bool communities) {
   QVariantList list;
   for (const auto &value : array) {
     if (!value.isObject()) continue;
     const auto obj=value.toObject();
     const QString id=obj.value(QStringLiteral("id")).toString();
     const QString name=obj.value(QStringLiteral("name")).toString();
     const QString kind=obj.value(QStringLiteral("channel_kind")).toString();
     if (!validId(id) || name.isEmpty() || name.size()>100 ||
         (!communities && kind!=QStringLiteral("voice"))) continue;
     list.append(QVariantMap{{QStringLiteral("id"),id},{QStringLiteral("name"),name}});
   }
   return list;
 }
 bool includes(const QVariantList &items,const QString &id) {
   for (const auto &item:items) if (item.toMap().value(QStringLiteral("id")).toString()==id) return true;
   return false;
 }
}
ChannelDirectory::ChannelDirectory(AccountSession *account,QObject *parent):QObject(parent),account_(account) {
 connect(account_,&AccountSession::signedOut,this,&ChannelDirectory::reset);
 connect(account_,&AccountSession::changed,this,[this]() {
   if (account_->signedIn() && communities_.isEmpty() && !busy()) refresh();
 });
}
void ChannelDirectory::reset() {
 ++version_;
 if (pending_) { auto *reply=pending_; pending_=nullptr; reply->abort(); reply->deleteLater(); }
 communities_.clear(); channels_.clear(); communityId_.clear(); channelId_.clear();
 status_=QStringLiteral("Sign in to view communities");
 emit changed(); emit selectionChanged();
}
void ChannelDirectory::refresh() {
 if (!account_->signedIn() || busy()) return;
 communityId_.clear(); channelId_.clear(); channels_.clear(); communities_.clear();
 emit selectionChanged();
 request(QStringLiteral("/v1/communities"),true);
}
void ChannelDirectory::chooseCommunity(const QString &id) {
 if (busy() || !account_->signedIn() || !includes(communities_,id)) return;
 communityId_=id; channelId_.clear(); channels_.clear();
 emit selectionChanged(); emit changed();
 request(QStringLiteral("/v1/communities/%1/channels").arg(id),false);
}
void ChannelDirectory::chooseVoiceChannel(const QString &id) {
 if (busy() || !includes(channels_,id) || communityId_.isEmpty()) return;
 channelId_=id;
 emit selectionChanged(); emit changed();
}
void ChannelDirectory::request(const QString &path,bool listCommunities) {
 auto url=account_->apiOrigin();
 if (url.scheme()!=QStringLiteral("https") &&
     !(url.scheme()==QStringLiteral("http") &&
       (url.host()==QStringLiteral("localhost") || url.host()==QStringLiteral("127.0.0.1")))) return;
 url.setPath(path);
 QNetworkRequest request(url);
 request.setRawHeader("Authorization",QByteArray("Bearer ")+account_->accessToken().toUtf8());
 request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
 request.setTransferTimeout(10000);
 status_=QStringLiteral("Loading…");
 pending_=network_.get(request);
 emit changed();
 auto *reply=pending_;
 auto const version=version_;
 connect(reply,&QNetworkReply::finished,this,[this,reply,version,listCommunities]() {
   if (version!=version_ || reply!=pending_) { reply->deleteLater(); return; }
   pending_=nullptr;
   const int code=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
   const auto error=reply->error();
   const auto bytes=reply->readAll();
   reply->deleteLater();
   QJsonParseError parse;
   const auto doc=QJsonDocument::fromJson(bytes,&parse);
   if (error!=QNetworkReply::NoError || code!=200 || bytes.size()>131072 ||
       parse.error!=QJsonParseError::NoError || !doc.isArray() || doc.array().size()>200) {
     status_=QStringLiteral("Unable to load channels");
     emit changed();
     return;
   }
   if (listCommunities) {
     communities_=rows(doc.array(),true);
     status_=communities_.isEmpty() ? QStringLiteral("No communities") : QStringLiteral("Choose a community");
   } else {
     channels_=rows(doc.array(),false);
     status_=channels_.isEmpty() ? QStringLiteral("No voice channels") : QStringLiteral("Choose a voice channel");
   }
   emit changed();
 });
}
