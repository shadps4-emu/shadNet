// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_presence.h"

#include <QByteArray>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {
namespace {

// Shared handler for the self-scoped presence-write PUTs (inGamePresence, gameStatus).
// There is no REST presence sink yet (presence is broadcast over the binary path), so these
// accept-and-acknowledge the write with 204 and log the payload for later wiring into the
// presence broadcast.
QHttpServerResponse HandlePresenceWrite(Database& db, const char* leaf, const QString& userKey,
                                        const QHttpServerRequest& req) {
    static const QSet<QString> kKnown = {
        QStringLiteral("notificationWithData"),
    };
    LogUnsupportedQueryParams(req, kKnown);

    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const bool self = userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
                      userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
                      userKey == QString::number(*auth.userId);
    if (!self) {
        return JsonError(QHttpServerResponse::StatusCode::Forbidden, UP_ACCESS_DENIED_OWNERSHIP,
                         QStringLiteral("Access denied by resource ownership"));
    }

    // Best-effort parse of the presence body so it shows up in the log; a malformed or empty
    // body is not an error for an accept-and-acknowledge write.
    const QByteArray body = req.body();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error == QJsonParseError::NoError && doc.isObject()) {
        qInfo() << "WebAPI:" << leaf << "for" << auth.npid << "->"
                << QString::fromUtf8(QJsonDocument(doc.object()).toJson(QJsonDocument::Compact));
    } else {
        qInfo() << "WebAPI:" << leaf << "for" << auth.npid << "(" << body.size()
                << "bytes, unparsed)";
    }

    // presence writes return 204 No Content.
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

} // namespace

void RegisterPresenceRoutes(QHttpServer& http, Database& db) {
    // PUT /v1/users/<arg>/presence/inGamePresence
    http.route("/v1/users/<arg>/presence/inGamePresence", QHttpServerRequest::Method::Put,
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceWrite(db, "inGamePresence", userKey, req);
               });

    // PUT /v1/users/<arg>/presence/gameStatus
    http.route("/v1/users/<arg>/presence/gameStatus", QHttpServerRequest::Method::Put,
               [&db](const QString& userKey, const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceWrite(db, "gameStatus", userKey, req);
               });
}

} // namespace WebApiRoutes
