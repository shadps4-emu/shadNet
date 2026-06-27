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
}

} // namespace WebApiRoutes
