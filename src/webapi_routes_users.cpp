// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_users.h"

#include <algorithm>
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
#include <QReadWriteLock>
#include <QUrlQuery>

#include "client_session.h" // SharedState
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

// friendList entries nest identity in user:{onlineId, accountId} (same shape as block/
// profile). shadNet stores only (accountId, npid) per relationship, so
// onlineId and personalDetail.displayName are both the npid; avatarUrl is looked up from
// the account table; isOfficiallyVerified defaults to false.
QJsonObject BuildFriendList(Database& db, SharedState& shared,
                            QList<QPair<int64_t, QString>> friends, // by value: sorted/filtered
                            const QStringList& fields, bool isDefault, int offset, int limit,
                            bool wantPresence, const QString& presenceType, bool presenceDetail,
                            const QString& filter, const QString& sortKey,
                            const QString& direction, int64_t callerUserId) {
    // @default == user,region,npId. personalDetail / avatarUrl / isOfficiallyVerified are NOT
    // in @default and must be requested explicitly.
    const bool wantUser = isDefault || fields.contains(QStringLiteral("user"));
    const bool wantRegion = isDefault || fields.contains(QStringLiteral("region"));
    const bool wantNpId = isDefault || fields.contains(QStringLiteral("npId"));
    const bool wantDetail = fields.contains(QStringLiteral("personalDetail")) ||
                            fields.contains(QStringLiteral("personalDetail.displayName"));
    const bool wantAvatar = fields.contains(QStringLiteral("avatarUrl")) ||
                            fields.contains(QStringLiteral("avatarUrls"));
    const bool wantVerified = fields.contains(QStringLiteral("isOfficiallyVerified"));

    // Snapshot each friend's presence (online == membership in the shared clients map, minus
    // Appear-Offline, plus the in-game detail the presence PUTs stored). Also capture comId
    // (for incontext / same-game) under the separate usageLock. Done once up front so we don't
    // hold a lock across the per-entry DB lookups below.
    struct PresenceSnap {
        QString gameStatus;
        QString gameData;
        QString npTitleId;
        QString titleName;
        QString platform;
        QString comId;
    };
    QHash<int64_t, PresenceSnap> onlineFriends;
    QString callerComId;
    if (wantPresence) {
        {
            QReadLocker lk(&shared.clientsLock);
            for (const auto& fr : friends) {
                auto it = shared.clients.find(fr.first);
                if (it != shared.clients.end() && !it->appearOffline) {
                    onlineFriends.insert(fr.first, {it->gameStatus, it->gameData, it->npTitleId,
                                                    it->titleName, it->platform, QString()});
                }
            }
        }
        {
            QReadLocker ul(&shared.usageLock);
            callerComId = shared.usageClientGame.value(callerUserId);
            for (auto sit = onlineFriends.begin(); sit != onlineFriends.end(); ++sit)
                sit.value().comId = shared.usageClientGame.value(sit.key());
        }
    }
    const bool inSameGame_caller = !callerComId.isEmpty();

    // filter=online -> only online friends; filter=incontext -> only same-comId friends.
    if (wantPresence && (filter == QStringLiteral("online") ||
                         filter == QStringLiteral("incontext"))) {
        QList<QPair<int64_t, QString>> kept;
        for (const auto& fr : friends) {
            const auto snap = onlineFriends.constFind(fr.first);
            const bool online = (snap != onlineFriends.constEnd());
            if (filter == QStringLiteral("online")) {
                if (online) kept.append(fr);
            } else { // incontext
                if (online && inSameGame_caller && snap->comId == callerComId)
                    kept.append(fr);
            }
        }
        friends = kept;
    }

    // sort=onlineId | onlineStatus | onlineStatus+onlineId ; direction asc|desc.
    if (!sortKey.isEmpty()) {
        const bool desc = (direction == QStringLiteral("desc"));
        const bool byStatus = sortKey.startsWith(QStringLiteral("onlineStatus"));
        const bool thenId = sortKey == QStringLiteral("onlineStatus+onlineId");
        auto rank = [&](int64_t id) { // 0 = online, 1 = offline (asc puts online first)
            return onlineFriends.contains(id) ? 0 : 1;
        };
        std::stable_sort(friends.begin(), friends.end(),
                         [&](const QPair<int64_t, QString>& a, const QPair<int64_t, QString>& b) {
                             if (byStatus) {
                                 const int ra = rank(a.first), rb = rank(b.first);
                                 if (ra != rb)
                                     return desc ? ra > rb : ra < rb;
                                 if (!thenId)
                                     return false; // stable: keep relative order
                             }
                             const int c = a.second.compare(b.second, Qt::CaseInsensitive);
                             return desc ? c > 0 : c < 0;
                         });
    }
    const int total = static_cast<int>(friends.size());

    QJsonArray arr;
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        const int64_t accountId = friends[i].first;
        const QString& npid = friends[i].second;
        QJsonObject entry;
        if (wantUser) {
            QJsonObject user;
            user.insert(QStringLiteral("onlineId"), npid);
            user.insert(QStringLiteral("accountId"), QString::number(accountId));
            entry.insert(QStringLiteral("user"), user);
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
        if (wantPresence) {
            const auto snap = onlineFriends.constFind(accountId);
            const bool online = (snap != onlineFriends.constEnd());
            const bool sameGame =
                online && inSameGame_caller && snap->comId == callerComId;
            const QString type =
                presenceType.isEmpty() ? QStringLiteral("primary") : presenceType;
            entry.insert(
                QStringLiteral("presence"),
                online ? MakePresence(type, true, snap->platform, snap->gameStatus,
                                      snap->gameData, snap->npTitleId, snap->titleName,
                                      presenceDetail, sameGame, QString())
                       : MakePresence(type, false, {}, {}, {}, {}, {}, presenceDetail, false,
                                      QString()));
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
// friendStatus=friend (required, only legal value). fields=[@default,user,region,npId,
//   personalDetail,personalDetail.displayName,avatarUrl,avatarUrls,isOfficiallyVerified]
//   (@default == user,region,npId; default when omitted; unknown field names ignored).
//   limit=[0-500] (default 100; 0 == count-only), offset=[0-2147483647] (default 0).
//   presenceType=[primary,platform,incontext] adds the presence member; presenceDetail=true
//   adds detail (ignored without presenceType); filter=[online,incontext]; sort=[onlineId,
//   onlineStatus,onlineStatus+onlineId] (onlineStatus* require presenceType) with
//   direction=[asc,desc]. Self-only: the path user must resolve to the authenticated
//   account. Ignored (logged when present): avatarSize(s), avatarUrlScheme,
//   profilePictureSizes, npLanguages.
//
// GET /v1/users/<accountId|onlineId|me>/blockList
//
// Supported: fields=[@default,user,npId] (@default == user; default when omitted),
//   limit=[0-2000] (default 2000; 0 == count-only), offset=[0-2147483647] (default 0).
// Block list is always self-only ("can only be obtained for the current user").
void RegisterUserRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // GET /v1/friendOnlineIdLists?accountIds=id1,id2,... (max 10)
    // Batch: each specified user's full friend list as account info only (onlineId +
    // accountId) -- no presence/profile (use /v1/users/<arg>/friendList for those).
    http.route(
        "/v1/friendOnlineIdLists",
        [&db](const QHttpServerRequest& req) -> QHttpServerResponse {
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value()) {
                return std::move(auth.errorResponse);
            }
            const QUrlQuery query(req.url());
            if (!query.hasQueryItem(QStringLiteral("accountIds"))) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_QUERY_PARAM_REQUIRED,
                    QStringLiteral("'accountIds' parameter required in query string"));
            }
            const QStringList ids =
                query.queryItemValue(QStringLiteral("accountIds"))
                    .split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (ids.isEmpty() || ids.size() > 10) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                    QStringLiteral("Invalid parameter in query string (parameter: 'accountIds')"));
            }
            QJsonArray lists;
            for (const QString& idStr : ids) {
                bool ok = false;
                const qlonglong accId = idStr.toLongLong(&ok);
                if (!ok)
                    continue; // skip malformed ids
                QJsonObject userObj;
                QJsonObject u;
                u.insert(QStringLiteral("onlineId"),
                         db.GetUsername(accId).value_or(QString()));
                u.insert(QStringLiteral("accountId"), QString::number(accId));
                userObj.insert(QStringLiteral("user"), u);
                QJsonArray fl;
                for (const auto& fr : db.GetRelationships(accId).friends) {
                    QJsonObject f;
                    f.insert(QStringLiteral("onlineId"), fr.second);
                    f.insert(QStringLiteral("accountId"), QString::number(fr.first));
                    fl.append(f);
                }
                userObj.insert(QStringLiteral("friendOnlineIdLists"), fl);
                lists.append(userObj);
            }
            QJsonObject body;
            body.insert(QStringLiteral("friendOnlineIdLists"), lists);
            qInfo() << "WebAPI: friendOnlineIdLists for" << ids.size() << "user(s)";
            return JsonOk(body);
        });

    http.route(
        "/v1/users/<arg>/friendList",
        [&db, &shared](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {
                QStringLiteral("friendStatus"),
                QStringLiteral("fields"),
                QStringLiteral("limit"),
                QStringLiteral("offset"),
                QStringLiteral("presenceType"),
                QStringLiteral("presenceDetail"),
                QStringLiteral("filter"),
                QStringLiteral("sort"),
                QStringLiteral("direction"),
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

            // presenceType selects the presence member; presence is included only when
            // presenceType is given (presenceDetail alone is ignored per the SDK).
            const QString presenceType = query.queryItemValue(QStringLiteral("presenceType"));
            const bool wantPresence = !presenceType.isEmpty();
            if (!presenceType.isEmpty() && presenceType != QStringLiteral("primary") &&
                presenceType != QStringLiteral("platform") &&
                presenceType != QStringLiteral("incontext")) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                    QStringLiteral("Invalid parameter in query string (parameter: 'presenceType')"));
            }
            // presenceDetail ignored when presenceType is absent.
            const bool presenceDetail =
                wantPresence &&
                query.queryItemValue(QStringLiteral("presenceDetail")) == QStringLiteral("true");
            const QString filter = query.queryItemValue(QStringLiteral("filter"));
            const QString sortKey = query.queryItemValue(QStringLiteral("sort"));
            const QString direction = query.queryItemValue(QStringLiteral("direction"));
            // Per the SDK, onlineStatus / onlineStatus+onlineId sorts may be used only when
            // presenceType is specified; otherwise the request is rejected.
            if ((sortKey == QStringLiteral("onlineStatus") ||
                 sortKey == QStringLiteral("onlineStatus+onlineId")) &&
                !wantPresence) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                    QStringLiteral("Invalid parameter in query string (parameter: 'sort')"));
            }
            const auto friends = db.GetRelationships(*auth.userId).friends;
            const QJsonObject body =
                BuildFriendList(db, shared, friends, fields, isDefault, offset, limit,
                                wantPresence, presenceType, presenceDetail, filter, sortKey,
                                direction, *auth.userId);
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
