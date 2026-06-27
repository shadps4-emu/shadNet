// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_users.h"

#include <optional>

#include <QByteArray>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {
namespace {

// The authenticated user owns the resource when the path segment is "me", their
// onlineId, or their numeric accountId. Both friendList and blockList are self-only.
bool IsSelf(const QString& userKey, const WebApiAuth::AuthResult& auth) {
    return userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
           userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
           userKey == QString::number(*auth.userId);
}

// Build a {<key>:[entry...], start, size, totalResults} body from (accountId, onlineId)
// pairs, emitting only the requested members. region is omitted unless wantRegion.
QJsonObject BuildUserList(const QList<QPair<int64_t, QString>>& users, const QString& key,
                          bool wantUser, bool wantRegion, bool wantNpId, int offset, int limit) {
    const int total = static_cast<int>(users.size());
    QJsonArray arr;
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        const auto& u = users[i]; // (accountId, onlineId)
        QJsonObject entry;
        if (wantUser) {
            QJsonObject user;
            user.insert(QStringLiteral("onlineId"), u.second);
            user.insert(QStringLiteral("accountId"), QString::number(u.first));
            entry.insert(QStringLiteral("user"), user);
        }
        if (wantRegion) {
            entry.insert(QStringLiteral("region"), QString::fromLatin1(DefaultRegion));
        }
        if (wantNpId) {
            entry.insert(QStringLiteral("npId"), EncodeNpId(u.second));
        }
        arr.append(entry);
    }
    QJsonObject body;
    body.insert(key, arr);
    body.insert(QStringLiteral("start"), offset);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    body.insert(QStringLiteral("totalResults"), total);
    return body;
}

// friendList entries carry the requested members directly (not wrapped in user:{} like
// the block/profile shape). shadNet stores only (accountId, npid) per relationship, so
// onlineId and personalDetail.displayName are both the npid; avatarUrl is looked up from
// the account table; isOfficiallyVerified defaults to false.
QJsonObject BuildFriendList(Database& db, const QList<QPair<int64_t, QString>>& friends,
                            const QStringList& fields, bool isDefault, int offset, int limit) {
    const bool wantOnlineId = isDefault || fields.contains(QStringLiteral("onlineId"));
    const bool wantRegion = isDefault || fields.contains(QStringLiteral("region"));
    const bool wantDetail = isDefault || fields.contains(QStringLiteral("personalDetail")) ||
                            fields.contains(QStringLiteral("personalDetail.displayName"));
    const bool wantAvatar = isDefault || fields.contains(QStringLiteral("avatarUrl"));
    const bool wantVerified = fields.contains(QStringLiteral("isOfficiallyVerified"));
    const bool wantNpId = isDefault || fields.contains(QStringLiteral("npId"));

    const int total = static_cast<int>(friends.size());
    QJsonArray arr;
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        const int64_t accountId = friends[i].first;
        const QString& npid = friends[i].second;
        QJsonObject entry;
        if (wantOnlineId) {
            entry.insert(QStringLiteral("onlineId"), npid);
        }
        if (wantNpId) {
            entry.insert(QStringLiteral("npId"), EncodeNpId(npid));
        }
        if (wantRegion) {
            entry.insert(QStringLiteral("region"), QString::fromLatin1(DefaultRegion));
        }
        if (wantDetail) {
            QJsonObject pd;
            pd.insert(QStringLiteral("displayName"), npid);
            entry.insert(QStringLiteral("personalDetail"), pd);
        }
        if (wantAvatar) {
            const auto avatar = db.GetAvatarUrl(accountId);
            if (avatar && !avatar->isEmpty()) {
                entry.insert(QStringLiteral("avatarUrl"), *avatar);
            }
        }
        if (wantVerified) {
            entry.insert(QStringLiteral("isOfficiallyVerified"), false);
        }
        arr.append(entry);
    }
    QJsonObject body;
    body.insert(QStringLiteral("friendList"), arr);
    body.insert(QStringLiteral("start"), offset);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    body.insert(QStringLiteral("totalResults"), total);
    return body;
}

} // namespace

// GET /v1/users/<accountId|onlineId|me>/friendList
//
// Supported: friendStatus=friend (required, only legal value), fields=[@default,user,
//   region,npId] (@default == user,region,npId; default when omitted), limit=[0-500]
//   (default 100; 0 == count-only), offset=[0-2147483647] (default 0).
// NOT yet supported (logged when present): presenceType, presenceDetail, filter, sort,
//   direction, avatarSize, avatarSizes, avatarUrlScheme, profilePictureSizes, npLanguages.
//   Unsupported fields values (avatarUrl(s), personalDetail*, isOfficiallyVerified) are
//   dropped. Self-only: the path user must resolve to the authenticated account.
//
// GET /v1/users/<accountId|onlineId|me>/blockList
//
// Supported: fields=[@default,user,npId] (@default == user; default when omitted),
//   limit=[0-2000] (default 2000; 0 == count-only), offset=[0-2147483647] (default 0).
// Block list is always self-only ("can only be obtained for the current user").
void RegisterUserRoutes(QHttpServer& http, Database& db) {
    http.route(
        "/v1/users/<arg>/friendList",
        [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {
                QStringLiteral("friendStatus"),
                QStringLiteral("fields"),
                QStringLiteral("limit"),
                QStringLiteral("offset"),
            };
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value()) {
                return std::move(auth.errorResponse);
            }
            if (!IsSelf(userKey, auth)) {
                return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                 UP_ACCESS_DENIED_OWNERSHIP,
                                 QStringLiteral("Access denied by resource ownership"));
            }

            const QUrlQuery query(req.url());

            // friendStatus is required and its only legal value is "friend".
            if (!query.hasQueryItem(QStringLiteral("friendStatus"))) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_QUERY_PARAM_REQUIRED,
                    QStringLiteral("'friendStatus' parameter required in query string"));
            }
            if (query.queryItemValue(QStringLiteral("friendStatus")) != QStringLiteral("friend")) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                    QStringLiteral(
                        "Invalid parameter in query string (parameter: 'friendStatus')"));
            }

            // fields: @default == user,region,npId.
            QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
            if (fieldsStr.isEmpty()) {
                fieldsStr = QStringLiteral("@default");
            }
            const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
            const bool isDefault = fields.contains(QStringLiteral("@default"));

            int limit = 0;
            int offset = 0;
            ParsePaging(query, 100, 500, limit, offset);

            const auto friends = db.GetRelationships(*auth.userId).friends;
            const QJsonObject body = BuildFriendList(db, friends, fields, isDefault, offset, limit);
            qInfo() << "WebAPI: friendList for" << auth.npid << "-> total" << friends.size();
            return JsonOk(body);
        });

    http.route("/v1/users/<arg>/blockList",
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("fields"),
                       QStringLiteral("limit"),
                       QStringLiteral("offset"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   if (!IsSelf(userKey, auth)) {
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        UP_ACCESS_DENIED_OWNERSHIP,
                                        QStringLiteral("Access denied by resource ownership"));
                   }

                   const QUrlQuery query(req.url());

                   // fields: @default == user (no region member for blockingUser).
                   QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
                   if (fieldsStr.isEmpty()) {
                       fieldsStr = QStringLiteral("@default");
                   }
                   const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
                   const bool isDefault = fields.contains(QStringLiteral("@default"));

                   int limit = 0;
                   int offset = 0;
                   ParsePaging(query, 2000, 2000, limit, offset);

                   const auto blocked = db.GetRelationships(*auth.userId).blocked;
                   const QJsonObject body =
                       BuildUserList(blocked, QStringLiteral("blockList"),
                                     isDefault || fields.contains(QStringLiteral("user")),
                                     /*wantRegion=*/false, fields.contains(QStringLiteral("npId")),
                                     offset, limit);
                   qInfo() << "WebAPI: blockList for" << auth.npid << "-> total" << blocked.size();
                   return JsonOk(body);
               });

    // GET /v1/users/<accountId|onlineId|me>/blockingUsers
    // Distinct endpoint from blockList; returns the users this account blocks. Self-only.
    http.route("/v1/users/<arg>/blockingUsers",
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("fields"),
                       QStringLiteral("limit"),
                       QStringLiteral("offset"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   if (!IsSelf(userKey, auth)) {
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        UP_ACCESS_DENIED_OWNERSHIP,
                                        QStringLiteral("Access denied by resource ownership"));
                   }

                   const QUrlQuery query(req.url());

                   QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
                   if (fieldsStr.isEmpty()) {
                       fieldsStr = QStringLiteral("@default");
                   }
                   const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
                   const bool isDefault = fields.contains(QStringLiteral("@default"));

                   int limit = 0;
                   int offset = 0;
                   ParsePaging(query, 2000, 2000, limit, offset);

                   const auto blocked = db.GetRelationships(*auth.userId).blocked;
                   const QJsonObject body =
                       BuildUserList(blocked, QStringLiteral("blockingUsers"),
                                     isDefault || fields.contains(QStringLiteral("user")),
                                     /*wantRegion=*/false, fields.contains(QStringLiteral("npId")),
                                     offset, limit);
                   qInfo() << "WebAPI: blockingUsers for" << auth.npid << "-> total"
                           << blocked.size();
                   return JsonOk(body);
               });
}

} // namespace WebApiRoutes
