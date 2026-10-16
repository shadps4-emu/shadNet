// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_commerce.h"

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

void RegisterCommerceRoutes(QHttpServer& http, Database& db) {
    // GET /v5/container?containerIds=<a[,b,...]>&serviceLabel=<n>
    http.route("/v5/container", QHttpServerRequest::Method::Get,
               [&db](const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("containerIds"),
                       QStringLiteral("serviceLabel"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }

                   const QUrlQuery query(req.url());
                   const QStringList ids = query.queryItemValue(QStringLiteral("containerIds"))
                                               .split(QLatin1Char(','), Qt::SkipEmptyParts);
                   if (ids.isEmpty()) {
                       return JsonError(QHttpServerResponse::StatusCode::BadRequest,
                                        UP_QUERY_PARAM_REQUIRED,
                                        QStringLiteral("'containerIds' parameter required in "
                                                       "query string"));
                   }

                   // Unknown containers are omitted, matching the batch profile lookup.
                   QJsonObject body;
                   body.insert(QStringLiteral("containers"), QJsonArray());
                   body.insert(QStringLiteral("size"), 0);
                   qInfo() << "WebAPI: container lookup" << ids << "-> none available";
                   return JsonOk(body);
               });
}

} // namespace WebApiRoutes
