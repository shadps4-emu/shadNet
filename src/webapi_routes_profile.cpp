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

#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {
namespace {

// GET /v1/profiles?onlineId=<a[,b,...]>
QJsonObject BuildProfiles(Database& db, const QStringList& onlineIds, const QStringList& fields,
                          bool isDefault) {
    const bool wantOnlineId = isDefault || fields.contains(QStringLiteral("onlineId"));
    const bool wantNpId = isDefault || fields.contains(QStringLiteral("npId"));
    const bool wantAvatar = isDefault || fields.contains(QStringLiteral("avatarUrl"));

    QJsonArray arr;
    for (const QString& requested : onlineIds) {
        const auto uid = db.GetUserId(requested);
        if (!uid) {
            continue; // unknown handle -- skip rather than fail the whole request
        }
        const QString onlineId = db.GetUsername(*uid).value_or(requested);
        QJsonObject entry;
        if (wantOnlineId) {
            entry.insert(QStringLiteral("onlineId"), onlineId);
        }
        if (wantNpId) {
            entry.insert(QStringLiteral("npId"), EncodeNpId(onlineId));
        }
        if (wantAvatar) {
            const auto avatar = db.GetAvatarUrl(*uid);
            if (avatar && !avatar->isEmpty()) {
                entry.insert(QStringLiteral("avatarUrl"), *avatar);
            }
        }
        arr.append(entry);
    }
    QJsonObject body;
    body.insert(QStringLiteral("profiles"), arr);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    return body;
}

// GET /v1/users/{userId}/profile -- a single user's Profile object
QJsonObject BuildProfile(Database& db, qint64 userId, const QString& onlineId,
                         const QStringList& fields, bool isDefault) {
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
        p.insert(QStringLiteral("personalDetail"), "it's me wanna fight?"); // unsupported
    }
    if (wantVerified) {
        p.insert(QStringLiteral("isOfficiallyVerified"), false);
    }
    return p;
}

} // namespace

void RegisterProfileRoutes(QHttpServer& http, Database& db) {
    // GET /v1/profiles?onlineId=<a[,b,...]>&fields=...
    http.route("/v1/profiles", [&db](const QHttpServerRequest& req) -> QHttpServerResponse {
        static const QSet<QString> kKnown = {
            QStringLiteral("onlineId"),
            QStringLiteral("fields"),
        };
        LogUnsupportedQueryParams(req, kKnown);

        auto auth = WebApiAuth::Authenticate(req, db);
        if (!auth.userId.has_value()) {
            return std::move(auth.errorResponse);
        }

        const QUrlQuery query(req.url());
        if (!query.hasQueryItem(QStringLiteral("onlineId"))) {
            return JsonError(QHttpServerResponse::StatusCode::BadRequest, UP_QUERY_PARAM_REQUIRED,
                             QStringLiteral("'onlineId' parameter required in query string"));
        }
        const QStringList onlineIds = query.queryItemValue(QStringLiteral("onlineId"))
                                          .split(QLatin1Char(','), Qt::SkipEmptyParts);

        QString fieldsStr = query.queryItemValue(QStringLiteral("fields"));
        if (fieldsStr.isEmpty()) {
            fieldsStr = QStringLiteral("@default");
        }
        const QStringList fields = fieldsStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const bool isDefault = fields.contains(QStringLiteral("@default"));

        const QJsonObject body = BuildProfiles(db, onlineIds, fields, isDefault);
        qInfo() << "WebAPI: profiles lookup" << onlineIds << "-> returned"
                << body.value(QStringLiteral("size")).toInt();
        return JsonOk(body);
    });

    // GET /v1/users/<arg>/profile -- single user's profile.
    http.route("/v1/users/<arg>/profile",
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
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

                   const QJsonObject body = BuildProfile(db, userId, onlineId, fields, isDefault);
                   qInfo() << "WebAPI: profile for" << onlineId << "fields" << fields;
                   return JsonOk(body);
               });
}

} // namespace WebApiRoutes
