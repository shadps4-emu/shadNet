// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_presence.h"

#include <cstring>

#include <QByteArray>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrlQuery>

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

    // gameStatus is required for the gameStatus endpoint (optional for inGamePresence,
    // which may set gameStatus and/or gameData).
    if (std::strcmp(leaf, "gameStatus") == 0 && !obj.contains(QStringLiteral("gameStatus"))) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, UP_QUERY_PARAM_REQUIRED,
                         QStringLiteral("'gameStatus' is required in the request body"));
    }

    // SDK: localizedGameStatus may only be set alongside a default gameStatus.
    if (obj.contains(QStringLiteral("localizedGameStatus")) &&
        !obj.contains(QStringLiteral("gameStatus"))) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, UP_QUERY_PARAM_REQUIRED,
                         QStringLiteral("'gameStatus' (default) is required when "
                                        "'localizedGameStatus' is specified"));
    }

    // Write the published detail into the caller's live presence entry. Per the SDK the
    // status/data may be set even while Appear-Offline; it just isn't observable until the
    // user disables it -- so we store unconditionally but suppress the update events below.
    // notificationWithData is sticky: once specified it persists across subsequent update
    // events (cleared on game-end / offline / game-data DELETE), per the SDK.
    const bool reqNotify = QUrlQuery(req.url()).queryItemValue(
                               QStringLiteral("notificationWithData")) == QStringLiteral("true");
    bool updaterAppearOffline = false;
    bool notify = false;
    {
        QWriteLocker lk(&shared.clientsLock);
        auto it = shared.clients.find(*auth.userId);
        if (it != shared.clients.end()) {
            updaterAppearOffline = it->appearOffline;
            if (reqNotify)
                it->notifyWithData = true; // latch sticky mode
            notify = it->notifyWithData;
            // Body schema (both PUTs): gameStatus (string), gameData (base64 string,
            // inGamePresence only), localizedGameStatus ([{npLanguage, gameStatus}]).
            if (obj.contains(QStringLiteral("gameStatus")))
                it->gameStatus = obj.value(QStringLiteral("gameStatus")).toString();
            if (obj.contains(QStringLiteral("gameData")))
                it->gameData = obj.value(QStringLiteral("gameData")).toString();
            if (obj.contains(QStringLiteral("localizedGameStatus"))) {
                it->localizedGameStatus.clear();
                const QJsonArray loc =
                    obj.value(QStringLiteral("localizedGameStatus")).toArray();
                for (const QJsonValue& v : loc) {
                    const QJsonObject le = v.toObject();
                    const QString lang = le.value(QStringLiteral("npLanguage")).toString();
                    if (!lang.isEmpty())
                        it->localizedGameStatus.insert(
                            lang, le.value(QStringLiteral("gameStatus")).toString());
                }
            }
            it->presenceUpdatedAt = QDateTime::currentSecsSinceEpoch();
        }
    }

    // Appear-Offline: stored above, but no one can observe it -> emit nothing.
    if (updaterAppearOffline)
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};

    // SDK: game-status / game-data presence update events are received ONLY by users on a
    // title with the same NP Communication ID as the updater. Snapshot the updater's comId
    // and the set of users sharing it (usageLock), separately from clientsLock to avoid lock
    // nesting. If the updater's comId isn't known yet, fall back to all online friends.
    QString updaterComId;
    QSet<int64_t> sameComId;
    {
        QReadLocker ul(&shared.usageLock);
        updaterComId = shared.usageClientGame.value(*auth.userId);
        if (!updaterComId.isEmpty()) {
            for (auto it = shared.usageClientGame.cbegin(); it != shared.usageClientGame.cend();
                 ++it) {
                if (it.value() == updaterComId)
                    sameComId.insert(it.key());
            }
        }
    }

    // Collect online friends to notify (recipient npid + send), comId-gated when known.
    QList<QPair<QString, std::function<void(QByteArray)>>> recipients;
    {
        QReadLocker lk(&shared.clientsLock);
        auto it = shared.clients.constFind(*auth.userId);
        if (it != shared.clients.constEnd()) {
            for (auto fr = it->friends.cbegin(); fr != it->friends.cend(); ++fr) {
                const int64_t fid = fr.key();
                if (!updaterComId.isEmpty() && !sameComId.contains(fid))
                    continue; // SDK comId gate
                auto fit = shared.clients.constFind(fid);
                if (fit != shared.clients.constEnd() && fit->send)
                    recipients.append({fit->npid, fit->send});
            }
        }
    }

    // Emit the per-field presence update events (SDK): gameStatus -> np:service:presence:
    // gameStatus, gameData -> np:service:presence:gameData, both with NP service name
    // 'inGamePresence'. The JSON body ({"gameStatus":..} / {"gameData":..}) is included only
    // when notify (sticky notificationWithData) is set; otherwise the event fires with no
    // body (re-fetch). 'notify' was latched in the write block above.
    QList<QPair<QString, QByteArray>> events; // (dataType, body)
    if (obj.contains(QStringLiteral("gameStatus"))) {
        QByteArray body;
        if (notify) {
            QJsonObject o;
            o.insert(QStringLiteral("gameStatus"), obj.value(QStringLiteral("gameStatus")));
            body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        }
        events.append({QStringLiteral("np:service:presence:gameStatus"), body});
    }
    if (obj.contains(QStringLiteral("gameData"))) {
        QByteArray body;
        if (notify) {
            QJsonObject o;
            o.insert(QStringLiteral("gameData"), obj.value(QStringLiteral("gameData")));
            body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        }
        events.append({QStringLiteral("np:service:presence:gameData"), body});
    }

    static const QString kInGamePresence = QStringLiteral("inGamePresence");
    for (const auto& ev : events) {
        for (const auto& rcpt : recipients) {
            // from = updater, to = recipient (per SDK event content).
            const QByteArray pkt = ClientSession::BuildNotification(
                NotificationType::WebApiPushEvent,
                ClientSession::BuildWebApiPushPayload(kInGamePresence, 0, ev.first, ev.second,
                                                      auth.npid, rcpt.first));
            rcpt.second(pkt);
        }
    }

    // presence writes return 204 No Content.
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

} // namespace

// Shared DELETE handler for the presence delete routes (gameData, gameStatus). Self only.
// Clears the named field, emits a bodiless update event of `dataType` (deletion is a change)
// to same-comId friends unless Appear-Offline, and -- for gameData only -- resets the sticky
// notificationWithData latch (the SDK reset trigger is DELETE game data, not game status).
enum class DeleteField { GameData, GameStatus };
static QHttpServerResponse HandlePresenceDelete(Database& db, SharedState& shared,
                                                DeleteField field, const QString& dataType,
                                                const QString& userKey,
                                                const QHttpServerRequest& req) {
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

    bool appearOffline = false;
    {
        QWriteLocker lk(&shared.clientsLock);
        auto it = shared.clients.find(*auth.userId);
        if (it != shared.clients.end()) {
            appearOffline = it->appearOffline;
            if (field == DeleteField::GameData) {
                it->gameData.clear();
                it->notifyWithData = false; // SDK: DELETE game data resets notificationWithData
            } else {
                it->gameStatus.clear();
                it->localizedGameStatus.clear(); // localized variants follow the default status
            }
            it->presenceUpdatedAt = QDateTime::currentSecsSinceEpoch();
        }
    }
    qInfo() << "WebAPI: delete"
            << (field == DeleteField::GameData ? "gameData" : "gameStatus") << "for"
            << auth.npid;

    if (appearOffline)
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};

    // Same-comId snapshot (usageLock), then recipients (clientsLock) -- non-nested.
    QString updaterComId;
    QSet<int64_t> sameComId;
    {
        QReadLocker ul(&shared.usageLock);
        updaterComId = shared.usageClientGame.value(*auth.userId);
        if (!updaterComId.isEmpty()) {
            for (auto it = shared.usageClientGame.cbegin(); it != shared.usageClientGame.cend();
                 ++it) {
                if (it.value() == updaterComId)
                    sameComId.insert(it.key());
            }
        }
    }
    QList<QPair<QString, std::function<void(QByteArray)>>> recipients;
    {
        QReadLocker lk(&shared.clientsLock);
        auto it = shared.clients.constFind(*auth.userId);
        if (it != shared.clients.constEnd()) {
            for (auto fr = it->friends.cbegin(); fr != it->friends.cend(); ++fr) {
                const int64_t fid = fr.key();
                if (!updaterComId.isEmpty() && !sameComId.contains(fid))
                    continue;
                auto fit = shared.clients.constFind(fid);
                if (fit != shared.clients.constEnd() && fit->send)
                    recipients.append({fit->npid, fit->send});
            }
        }
    }
    // Deletion event carries no body (SDK: update events upon deletion omit the member).
    static const QString kInGamePresence = QStringLiteral("inGamePresence");
    for (const auto& rcpt : recipients) {
        const QByteArray pkt = ClientSession::BuildNotification(
            NotificationType::WebApiPushEvent,
            ClientSession::BuildWebApiPushPayload(kInGamePresence, 0, dataType, QByteArray(),
                                                  auth.npid, rcpt.first));
        rcpt.second(pkt);
    }
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

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

    // PUT /v1/users/<arg>/presence/gameData (body: {gameData} only)
    http.route("/v1/users/<arg>/presence/gameData", QHttpServerRequest::Method::Put,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceWrite(db, shared, "gameData", userKey, req);
               });

    // DELETE /v1/users/<arg>/presence/gameData
    http.route("/v1/users/<arg>/presence/gameData", QHttpServerRequest::Method::Delete,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceDelete(db, shared, DeleteField::GameData,
                                               QStringLiteral("np:service:presence:gameData"),
                                               userKey, req);
               });

    // DELETE /v1/users/<arg>/presence/gameStatus
    http.route("/v1/users/<arg>/presence/gameStatus", QHttpServerRequest::Method::Delete,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandlePresenceDelete(db, shared, DeleteField::GameStatus,
                                               QStringLiteral("np:service:presence:gameStatus"),
                                               userKey, req);
               });

    // GET /v1/users/<arg>/presence -- read a user's live presence. Self or a friend only
    // (non-friend -> 2107904). 'type' selects the member: primary -> primaryInfo,
    // platform -> platformInfoList, incontext -> incontextInfoList (same-NP-Comm-Id games).
    // presenceDetail=true gates gameStatus /
    // gameTitleInfo. Detail/online come from the presence PUTs + clients-map membership.
    http.route("/v1/users/<arg>/presence",
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("type"),
                       QStringLiteral("platform"),
                       QStringLiteral("presenceDetail"),
                       QStringLiteral("npLanguages"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }

                   const QUrlQuery query(req.url());

                   // 'type' is required: primary | platform | incontext.
                   const QString type = query.queryItemValue(QStringLiteral("type"));
                   if (type.isEmpty()) {
                       return JsonError(QHttpServerResponse::StatusCode::BadRequest,
                                        UP_QUERY_PARAM_REQUIRED,
                                        QStringLiteral("'type' parameter required in query string"));
                   }
                   if (type != QStringLiteral("primary") && type != QStringLiteral("platform") &&
                       type != QStringLiteral("incontext")) {
                       return JsonError(
                           QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                           QStringLiteral("Invalid parameter in query string (parameter: 'type')"));
                   }
                   // 'platform' is only legal with type=platform|incontext.
                   const QString platReq = query.queryItemValue(QStringLiteral("platform"));
                   if (!platReq.isEmpty() && type == QStringLiteral("primary")) {
                       return JsonError(
                           QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                           QStringLiteral("Invalid parameter in query string (parameter: 'platform')"));
                   }
                   const bool detail =
                       query.queryItemValue(QStringLiteral("presenceDetail")) ==
                       QStringLiteral("true");

                   // Resolve target: self, else by numeric accountId or onlineId.
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

                   // Presence is visible only to the owner or a friend.
                   bool allowed = self;
                   if (!allowed && targetId != 0) {
                       for (const auto& fr : db.GetRelationships(*auth.userId).friends) {
                           if (fr.first == targetId) {
                               allowed = true;
                               break;
                           }
                       }
                   }
                   if (!allowed) {
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        UP_NON_FRIEND_NOT_ALLOWED,
                                        QStringLiteral("Non-friend not allowed"));
                   }

                   bool online = false;
                   QString platform, gameStatus, gameData, npTitleId, titleName;
                   QHash<QString, QString> localized;
                   {
                       QReadLocker lk(&shared.clientsLock);
                       auto it = shared.clients.constFind(targetId);
                       // Appear-Offline target is reported offline with no in-game detail.
                       if (it != shared.clients.constEnd() && !it->appearOffline) {
                           online = true;
                           platform = it->platform;
                           gameStatus = it->gameStatus;
                           gameData = it->gameData;
                           npTitleId = it->npTitleId;
                           titleName = it->titleName;
                           localized = it->localizedGameStatus;
                       }
                   }
                   const QString onlineId = db.GetUsername(targetId).value_or(
                       self ? auth.npid : userKey);

                   // gameStatus is the only multilingual field: pick the first npLanguages
                   // match, else fall back to the default gameStatus.
                   const QString npLangs = query.queryItemValue(QStringLiteral("npLanguages"));
                   if (!npLangs.isEmpty() && !localized.isEmpty()) {
                       for (const QString& lang :
                            npLangs.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                           const auto lit = localized.constFind(lang);
                           if (lit != localized.constEnd()) {
                               gameStatus = lit.value();
                               break;
                           }
                       }
                   }

                   // incontext: does the target share the caller's NP Comm ID?
                   bool inSameGame = false;
                   if (online && type == QStringLiteral("incontext")) {
                       QReadLocker ul(&shared.usageLock);
                       const QString callerComId =
                           shared.usageClientGame.value(*auth.userId);
                       inSameGame = !callerComId.isEmpty() &&
                                    callerComId == shared.usageClientGame.value(targetId);
                   }
                   const QJsonObject presence =
                       MakePresence(type, online, platform, gameStatus, gameData, npTitleId,
                                    titleName, detail, inSameGame, platReq);

                   QJsonObject body;
                   body.insert(QStringLiteral("npId"), EncodeNpId(onlineId));
                   body.insert(QStringLiteral("presence"), presence);
                   qInfo() << "WebAPI: presence(" << type << ") for" << userKey
                           << (online ? "-> online" : "-> offline");
                   return JsonOk(body);
               });
}

} // namespace WebApiRoutes
