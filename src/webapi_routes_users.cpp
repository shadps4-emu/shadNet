// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_users.h"

#include <optional>

#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QUrlQuery>

#include "database.h"
#include "webapi_auth.h"

// Puyo Puyo Champions
//   GET /v1/users/%s/friendList?friendStatus=friend&fields=npId
//   GET /v1/users/%s/blockList?fields=npId&limit=%d   (limit=100)

namespace {

// wrapper key can be "friendList" or "blockList".
QHttpServerResponse BuildListResponse(const QList<QPair<int64_t, QString>>& source,
                                      const QString& wrapperKey, int limit) {
    const qsizetype total = source.size();
    const qsizetype emit_n = std::min<qsizetype>(limit, total);

    QJsonArray entries;
    for (qsizetype i = 0; i < emit_n; ++i) {
        QJsonObject entry;
        entry.insert("npId", source[i].second);
        entries.append(entry);
    }

    QJsonObject body;
    body.insert(wrapperKey, entries);
    body.insert("size", static_cast<qint64>(entries.size()));
    body.insert("start", 0);
    body.insert("totalResults", static_cast<qint64>(total));

    return QHttpServerResponse{
        "application/json",
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        QHttpServerResponse::StatusCode::Ok,
    };
}

// parse limit query param
int ParseLimit(const QHttpServerRequest& req) {
    constexpr int kDefault = 500;
    constexpr int kMax = 500;
    const QUrlQuery query(req.url());
    if (!query.hasQueryItem(QStringLiteral("limit"))) {
        return kDefault;
    }
    bool ok = false;
    const int v = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
    if (!ok || v <= 0) {
        return kDefault;
    }
    return std::min(v, kMax);
}

QHttpServerResponse JsonErrorReply(QHttpServerResponse::StatusCode status, quint32 code,
                                   const QString& message) {
    QJsonObject errorObj;
    errorObj.insert("code", static_cast<qint64>(code));
    errorObj.insert("message", message);
    QJsonObject body;
    body.insert("error", errorObj);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

QHttpServerResponse JsonOkReply(const QJsonObject& body) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

void LogUnsupportedQueryParams(const QHttpServerRequest& req, const QSet<QString>& known) {
    const QUrlQuery query(req.url());
    QStringList ignored;
    for (const auto& [key, value] : query.queryItems()) {
        if (!known.contains(key)) {
            ignored.append(QStringLiteral("%1=%2").arg(key, value));
        }
    }
    if (!ignored.isEmpty()) {
        qWarning() << "WebAPI:" << req.url().path()
                   << "ignored params:" << ignored.join(QStringLiteral(", "));
    }
}

std::optional<QString> ResolveUserSegment(const QString& segment,
                                          const WebApiAuth::AuthResult& auth, Database& db) {
    // `me` shortcut. Cheapest path — no DB hit.
    if (segment == QStringLiteral("me")) {
        if (auth.userId.has_value()) {
            return auth.npid;
        }
        return std::nullopt;
    }

    // Numeric accountId form: /v1/users/42/...
    // Reverse-look the npid so we can compare on the canonical name.
    bool numericOk = false;
    const qint64 asNumeric = segment.toLongLong(&numericOk);
    if (numericOk && asNumeric > 0) {
        return db.GetUsername(asNumeric);
    }

    const auto userIdOpt = db.GetUserId(segment);
    if (!userIdOpt.has_value()) {
        return std::nullopt;
    }
    return db.GetUsername(*userIdOpt);
}

} // namespace

namespace WebApiRoutes {

void RegisterUserRoutes(QHttpServer& http, Database& db) {
    // GET /v1/users/<userKey>/friendList
    http.route(
        "/v1/users/<arg>/friendList", [&db](const QString& userKey, const QHttpServerRequest& req) {
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
            const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
            if (!resolvedNpid.has_value()) {
                return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound, 0x80920004,
                                      QStringLiteral("User not found"));
            }
            if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden, 0x80920003,
                                      QStringLiteral("Forbidden"));
            }
            UserRelationships rels = db.GetRelationships(*auth.userId);
            return BuildListResponse(rels.friends, QStringLiteral("friendList"), ParseLimit(req));
        });

    // GET /v1/users/<userKey>/blockList
    http.route(
        "/v1/users/<arg>/blockList", [&db](const QString& userKey, const QHttpServerRequest& req) {
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
            const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
            if (!resolvedNpid.has_value()) {
                return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound, 0x80920004,
                                      QStringLiteral("User not found"));
            }
            if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden, 0x80920003,
                                      QStringLiteral("Forbidden"));
            }
            UserRelationships rels = db.GetRelationships(*auth.userId);
            return BuildListResponse(rels.blocked, QStringLiteral("blockList"), ParseLimit(req));
        });

    // GET /v1/users/<userKey>/container/<label>  (np_commerce2 product container)
    // Jetpack Joyride probes COINPACK / HIDDENINGAME at startup. We have no store
    // products yet, so return a well-formed, empty container so sce::Json parses it
    // cleanly and the title sees "no products" instead of a 404.
    http.route("/v1/users/<arg>/container/<arg>",
               [&db](const QString& userKey, const QString& label,
                     const QHttpServerRequest& req) {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("serviceLabel"), QStringLiteral("size"),
                       QStringLiteral("start"),        QStringLiteral("category"),
                       QStringLiteral("keepHtmlTag"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value()) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound,
                                             0x80920004, QStringLiteral("User not found"));
                   }
                   if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             0x80920003, QStringLiteral("Forbidden"));
                   }
                   qInfo() << "WebAPI: container" << label
                           << "-> empty (no products) for" << auth.npid;

                   QJsonObject data;
                   data.insert(QStringLiteral("items"), QJsonArray{});
                   QJsonObject body;
                   body.insert(QStringLiteral("size"), 0);
                   body.insert(QStringLiteral("start"), 0);
                   body.insert(QStringLiteral("totalResults"), 0);
                   body.insert(QStringLiteral("data"), data);
                   return JsonOkReply(body);
               });

    // GET /v1/users/<userKey>/entitlements  (owned DLC / service entitlements list)
    http.route("/v1/users/<arg>/entitlements",
               [&db](const QString& userKey, const QHttpServerRequest& req) {
                   static const QSet<QString> kKnown = {
                       QStringLiteral("service_label"),    QStringLiteral("entitlement_type"),
                       QStringLiteral("size"),             QStringLiteral("start"),
                       QStringLiteral("fields"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value()) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound,
                                             0x80920004, QStringLiteral("User not found"));
                   }
                   if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             0x80920003, QStringLiteral("Forbidden"));
                   }

                   QJsonObject body;
                   body.insert(QStringLiteral("start"), 0);
                   body.insert(QStringLiteral("size"), 0);
                   body.insert(QStringLiteral("totalResults"), 0);
                   body.insert(QStringLiteral("entitlements"), QJsonArray{});
                   return JsonOkReply(body);
               });

    // GET /v1/users/<userKey>/entitlements/<entitlementId>  (single entitlement lookup)
    // Nothing owned yet -> 404 "not found", which titles read as "not entitled".
    http.route("/v1/users/<arg>/entitlements/<arg>",
               [&db](const QString& userKey, const QString& entitlementId,
                     const QHttpServerRequest& req) {
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value() ||
                       resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             0x80920003, QStringLiteral("Forbidden"));
                   }
                   qInfo() << "WebAPI: entitlement" << entitlementId
                           << "not owned for" << auth.npid;
                   return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound,
                                         0x80920004, QStringLiteral("Entitlement not found"));
               });
}

} // namespace WebApiRoutes
