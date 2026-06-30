// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_profile.h"

#include <optional>

#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include "client_session.h" // SharedState
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {
namespace {

// GET /v1/profiles?onlineId=<a[,b,...]>
// Defined below; declared here so the batch builder can delegate to it.
QJsonObject BuildProfile(Database& db, SharedState& shared, qint64 userId, const QString& onlineId,
                         const QStringList& fields, bool isDefault, qint64 callerUserId);

// Batch profiles: accountIds are numeric account IDs. Each entry reuses the single-user
// builder so every field behaves identically.
QJsonObject BuildProfiles(Database& db, SharedState& shared, const QStringList& accountIds,
                          const QStringList& fields, bool isDefault, qint64 callerUserId) {
    QJsonArray arr;
    for (const QString& idStr : accountIds) {
        bool ok = false;
        const qlonglong accId = idStr.toLongLong(&ok);
        if (!ok)
            continue; // skip malformed ids
        const auto name = db.GetUsername(accId);
        if (!name)
            continue; // unknown account -- skip rather than fail the whole request
        arr.append(BuildProfile(db, shared, accId, *name, fields, isDefault, callerUserId));
    }
    QJsonObject body;
    body.insert(QStringLiteral("profiles"), arr);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    return body;
}

// GET /v1/users/{userId}/profile -- a single user's Profile object
QJsonObject BuildProfile(Database& db, SharedState& shared, qint64 userId, const QString& onlineId,
                         const QStringList& fields, bool isDefault, qint64 callerUserId) {
    const bool wantUser = isDefault || fields.contains(QStringLiteral("user"));
    const bool wantRegion = isDefault || fields.contains(QStringLiteral("region"));
    const bool wantNpId = isDefault || fields.contains(QStringLiteral("npId"));
    const bool wantAvatar = isDefault || fields.contains(QStringLiteral("avatarUrl")) ||
                            fields.contains(QStringLiteral("avatarUrls"));
    const bool wantAboutMe = isDefault || fields.contains(QStringLiteral("aboutMe"));
    const bool wantLanguages = isDefault || fields.contains(QStringLiteral("languagesUsed"));
    const bool wantDetail = fields.contains(QStringLiteral("personalDetail")) ||
                            fields.contains(QStringLiteral("personalDetail.displayName"));
    const bool wantVerified = fields.contains(QStringLiteral("isOfficiallyVerified"));
    const bool wantPresence = fields.contains(QStringLiteral("presence"));

    QJsonObject p;
    if (wantUser) {
        QJsonObject user;
        user.insert(QStringLiteral("onlineId"), onlineId);
        user.insert(QStringLiteral("accountId"), QString::number(userId));
        p.insert(QStringLiteral("user"), user);
    }
    if (wantRegion) {
        p.insert(QStringLiteral("region"), QString::fromLatin1(DefaultRegion));
    }
    if (wantNpId) {
        p.insert(QStringLiteral("npId"), EncodeNpId(onlineId));
    }
    if (wantAvatar) {
        const auto avatar = db.GetAvatarUrl(userId);
        if (avatar && !avatar->isEmpty()) {
            p.insert(QStringLiteral("avatarUrl"), *avatar);
        }
    }
    if (wantAboutMe) {
        p.insert(QStringLiteral("aboutMe"), QString()); // unsupported
    }
    if (wantLanguages) {
        p.insert(QStringLiteral("languagesUsed"), QJsonArray()); // unsupported
    }
    if (wantDetail) {
        QJsonObject pd;
        pd.insert(QStringLiteral("displayName"), onlineId);
        p.insert(QStringLiteral("personalDetail"), pd);
    }
    if (wantVerified) {
        p.insert(QStringLiteral("isOfficiallyVerified"), false);
    }
    if (wantPresence) {
        // Profile embeds presence as {primaryInfo: <entry>} -- note: unlike friendList and
        // GET presence, the profile presence object has no top-level onlineStatus member.
        bool actualOnline = false;
        bool tgtAppearOffline = false;
        QString platform, gameStatus, npTitleId, titleName;
        {
            QReadLocker lk(&shared.clientsLock);
            auto it = shared.clients.constFind(userId);
            if (it != shared.clients.constEnd()) {
                actualOnline = true;
                tgtAppearOffline = it->appearOffline;
                platform = it->platform;
                gameStatus = it->gameStatus;
                npTitleId = it->npTitleId;
                titleName = it->titleName;
            }
        }
        // Self always sees real status; a same-comId caller sees the target even when
        // Appear-Offline; gameStatus is obtainable only by self or same-comId callers
        // (consistent with friendList and GET presence).
        const bool self = (callerUserId == userId);
        bool sameGame = false;
        if (actualOnline && !self) {
            QReadLocker ul(&shared.usageLock);
            const QString callerComId = shared.usageClientGame.value(callerUserId);
            sameGame =
                !callerComId.isEmpty() && callerComId == shared.usageClientGame.value(userId);
        }
        const bool online = actualOnline && (self || sameGame || !tgtAppearOffline);
        const QString gs = (self || sameGame) ? gameStatus : QString();
        QJsonObject presence;
        presence.insert(QStringLiteral("primaryInfo"),
                        MakePresenceEntry(online, platform, gs, QString(), npTitleId, titleName,
                                          /*includeDetail=*/true,
                                          /*forcePlatform=*/false));
        p.insert(QStringLiteral("presence"), presence);
    }
    return p;
}

} // namespace

void RegisterProfileRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // GET /v1/profiles?onlineId=<a[,b,...]>&fields=...
    http.route(
        "/v1/profiles", [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {
                QStringLiteral("accountIds"),
                QStringLiteral("onlineId"),
                QStringLiteral("fields"),
                QStringLiteral("avatarSize"),
                QStringLiteral("avatarSizes"),
                QStringLiteral("avatarUrlScheme"),
                QStringLiteral("profilePictureSizes"),
                QStringLiteral("aboutMeType"),
                QStringLiteral("languagesUsedLanguageSet"),
                QStringLiteral("npLanguages"),
            };
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value()) {
                return std::move(auth.errorResponse);
            }

            const QUrlQuery query(req.url());
            // Accept either accountIds (numeric) or onlineId (handles, resolved to account IDs).
            // shadPS4's NpWebApi layer looks profiles up by onlineId.
            QStringList accountIds;
            if (query.hasQueryItem(QStringLiteral("accountIds"))) {
                accountIds = query.queryItemValue(QStringLiteral("accountIds"))
                                 .split(QLatin1Char(','), Qt::SkipEmptyParts);
                if (accountIds.isEmpty() || accountIds.size() > 50) {
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest,
                                     UP_INVALID_QUERY_PARAM,
                                     QStringLiteral("Invalid parameter in query string (parameter: "
                                                    "'accountIds')"));
                }
            } else if (query.hasQueryItem(QStringLiteral("onlineId"))) {
                const QStringList onlineIds = query.queryItemValue(QStringLiteral("onlineId"))
                                                  .split(QLatin1Char(','), Qt::SkipEmptyParts);
                if (onlineIds.isEmpty() || onlineIds.size() > 50) {
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest,
                                     UP_INVALID_QUERY_PARAM,
                                     QStringLiteral("Invalid parameter in query string (parameter: "
                                                    "'onlineId')"));
                }
                // Resolve each handle to an account ID; unknown handles are omitted from the
                // result (matching batch-lookup semantics).
                for (const QString& oid : onlineIds) {
                    const auto uid = db.GetUserId(oid);
                    if (uid)
                        accountIds.append(QString::number(*uid));
                }
            } else {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest,
                                 UP_QUERY_PARAM_REQUIRED,
                                 QStringLiteral("'accountIds' or 'onlineId' parameter required in "
                                                "query string"));
            }

            QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
            if (fieldsStr.isEmpty()) {
                fieldsStr = QStringLiteral("@default");
            }
            const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
            const bool isDefault = fields.contains(QStringLiteral("@default"));

            const QJsonObject body =
                BuildProfiles(db, shared, accountIds, fields, isDefault, *auth.userId);
            qInfo() << "WebAPI: profiles lookup" << accountIds.size() << "account(s) -> returned"
                    << body.value(QStringLiteral("size")).toInt();
            return JsonOk(body);
        });

    // GET /v1/users/<arg>/profile -- single user's profile.
    http.route("/v1/users/<arg>/profile",
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("fields"),
                       QStringLiteral("avatarSize"),
                       QStringLiteral("avatarSizes"),
                       QStringLiteral("avatarUrlScheme"),
                       QStringLiteral("profilePictureSizes"),
                       QStringLiteral("aboutMeType"),
                       QStringLiteral("languagesUsedLanguageSet"),
                       QStringLiteral("npLanguages"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const bool self =
                       userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
                       userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
                       userKey == QString::number(*auth.userId);
                   if (!self) {
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        UP_ACCESS_DENIED_OWNERSHIP,
                                        QStringLiteral("Access denied by resource ownership"));
                   }

                   const qint64 userId = *auth.userId;
                   const QString onlineId = db.GetUsername(userId).value_or(auth.npid);

                   const QUrlQuery query(req.url());
                   QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
                   if (fieldsStr.isEmpty()) {
                       fieldsStr = QStringLiteral("@default");
                   }
                   const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
                   const bool isDefault = fields.contains(QStringLiteral("@default"));

                   const QJsonObject body =
                       BuildProfile(db, shared, userId, onlineId, fields, isDefault, *auth.userId);
                   qInfo() << "WebAPI: profile for" << onlineId << "fields" << fields;
                   return JsonOk(body);
               });

    // GET /v1/users/<accountId|me>/profile/personalDetail/isAvailable -- whether the user
    // permits in-game real-name display. Self-only. isAvailable is present only when a real
    // name is registered; shadNet has no real-name registration, so we always return {} (the
    // "real name not registered" case).
    http.route("/v1/users/<arg>/profile/personalDetail/isAvailable",
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const bool self =
                       userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
                       userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
                       userKey == QString::number(*auth.userId);
                   if (!self) {
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        UP_ACCESS_DENIED_OWNERSHIP,
                                        QStringLiteral("Access denied by resource ownership"));
                   }
                   qInfo() << "WebAPI: personalDetail/isAvailable for" << auth.npid
                           << "-> {} (no real name registered)";
                   return JsonOk(QJsonObject{});
               });
}

} // namespace WebApiRoutes
