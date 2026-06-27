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

#include <QDateTime>
#include <QJsonValue>

#include "client_session.h" // SharedState, ClientSession push helpers, NotificationType
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {
namespace {

// Shared handler for the self-scoped presence-write PUTs (inGamePresence, gameStatus).
// There is no REST presence sink yet (presence is broadcast over the binary path), so these
// accept-and-acknowledge the write with 204 and log the payload for later wiring into the
// presence broadcast.
QHttpServerResponse HandlePresenceWrite(Database& db, SharedState& shared, const char* leaf,
                                        const QString& userKey, const QHttpServerRequest& req) {
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

    // Best-effort parse of the presence body; a malformed or empty body is not an error
    // for an accept-and-acknowledge write.
    const QByteArray body = req.body();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    const QJsonObject obj = (perr.error == QJsonParseError::NoError && doc.isObject())
                                ? doc.object()
                                : QJsonObject{};
    if (!obj.isEmpty()) {
        qInfo() << "WebAPI:" << leaf << "for" << auth.npid << "->"
                << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    } else {
        qInfo() << "WebAPI:" << leaf << "for" << auth.npid << "(" << body.size()
                << "bytes, unparsed)";
    }

    // Write the published detail into the caller's live presence entry, then collect the
    // online friends to notify. Online status is implicit by membership in the clients map;
    // these fields add the in-game detail that friendList?presenceType=... reads back.
    QList<std::function<void(QByteArray)>> friendSenders;
    {
        QWriteLocker lk(&shared.clientsLock);
        auto it = shared.clients.find(*auth.userId);
        if (it != shared.clients.end()) {
            if (obj.contains(QStringLiteral("gameStatus")))
                it->gameStatus = obj.value(QStringLiteral("gameStatus")).toString();
            if (obj.contains(QStringLiteral("npTitleId")))
                it->npTitleId = obj.value(QStringLiteral("npTitleId")).toString();
            if (obj.contains(QStringLiteral("titleName")))
                it->titleName = obj.value(QStringLiteral("titleName")).toString();
            if (obj.contains(QStringLiteral("platform")))
                it->platform = obj.value(QStringLiteral("platform")).toString();
            it->presenceUpdatedAt = QDateTime::currentSecsSinceEpoch();
            for (auto fr = it->friends.cbegin(); fr != it->friends.cend(); ++fr) {
                auto fit = shared.clients.find(fr.key());
                if (fit != shared.clients.end() && fit->send)
                    friendSenders.append(fit->send);
            }
        }
    }

    // Tell each online friend's push listener our presence changed; it re-fetches via
    // friendList?presenceType=... (same empty-body trigger the connect/disconnect path uses).
    if (!friendSenders.isEmpty()) {
        const QByteArray pkt = ClientSession::BuildNotification(
            NotificationType::WebApiPushEvent,
            ClientSession::BuildWebApiPushPayload(
                QString(), 0, QStringLiteral("np:service:presence:onlineStatus"), QByteArray(),
                auth.npid, QString()));
        for (const auto& send : friendSenders)
            send(pkt);
    }

    // presence writes return 204 No Content.
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

} // namespace

void RegisterPresenceRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // PUT /v1/users/<arg>/presence/inGamePresence
    http.route("/v1/users/<arg>/presence/inGamePresence", QHttpServerRequest::Method::Put,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceWrite(db, shared, "inGamePresence", userKey, req);
               });

    // PUT /v1/users/<arg>/presence/gameStatus
    http.route("/v1/users/<arg>/presence/gameStatus", QHttpServerRequest::Method::Put,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceWrite(db, shared, "gameStatus", userKey, req);
               });

    // GET /v1/users/<arg>/presence -- read a user's live presence. Resolves self or any
    // other user (by accountId or onlineId); returns offline when the target is not in the
    // shared clients map. Detail (gameStatus / gameTitleInfo) comes from the presence PUTs.
    http.route("/v1/users/<arg>/presence",
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("fields"),
                       QStringLiteral("platform"),
                       QStringLiteral("npLanguage"),
                       QStringLiteral("type"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }

                   // Resolve target: self ("me" / own onlineId / own accountId), else look
                   // up by numeric accountId or onlineId. Unresolvable -> id 0 -> offline.
                   int64_t targetId = 0;
                   const bool self =
                       userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
                       userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
                       userKey == QString::number(*auth.userId);
                   if (self) {
                       targetId = *auth.userId;
                   } else {
                       bool numeric = false;
                       const qlonglong asId = userKey.toLongLong(&numeric);
                       if (numeric) {
                           targetId = asId;
                       } else if (const auto byName = db.GetUserId(userKey)) {
                           targetId = *byName;
                       }
                   }

                   bool online = false;
                   QString platform, gameStatus, npTitleId, titleName;
                   {
                       QReadLocker lk(&shared.clientsLock);
                       auto it = shared.clients.constFind(targetId);
                       if (it != shared.clients.constEnd()) {
                           online = true;
                           platform = it->platform;
                           gameStatus = it->gameStatus;
                           npTitleId = it->npTitleId;
                           titleName = it->titleName;
                       }
                   }
                   qInfo() << "WebAPI: presence for" << userKey
                           << (online ? "-> online" : "-> offline");
                   return JsonOk(
                       MakePresenceObject(online, platform, gameStatus, npTitleId, titleName));
               });
}

} // namespace WebApiRoutes
