// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_identity.h"

#include <optional>
#include <utility>

#include <QDateTime>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

constexpr quint32 WEBAPI_RESOURCE_NOT_FOUND = 2113549; // target resource does not exist
constexpr quint32 WEBAPI_INVALID_BODY = 2113920;       // request body value invalid
constexpr quint32 WEBAPI_INVALID_NUM_ELEMS = 2113923;  // array element count invalid
constexpr qsizetype kMaxBatchIds = 100;                // ids accepted per batch mapping call

bool IsValidOnlineId(const QString& onlineId) {
    static const QRegularExpression re(
        QRegularExpression::anchoredPattern(QStringLiteral("[a-zA-Z][a-zA-Z0-9_-]{2,15}")));
    return re.match(onlineId).hasMatch();
}

std::optional<QDateTime> ParseAsOf(const QString& raw) {
    QString value = raw.trimmed();
    if (value.isEmpty()) {
        return std::nullopt;
    }
    static const QRegularExpression compactOffset(QStringLiteral("([+-])(\\d{2})(\\d{2})\\z"));
    const auto m = compactOffset.match(value);
    if (m.hasMatch()) {
        value.replace(m.capturedStart(), m.capturedLength(),
                      m.captured(1) + m.captured(2) + QLatin1Char(':') + m.captured(3));
    }
    QDateTime dt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(value, Qt::ISODate);
    }
    if (!dt.isValid()) {
        return std::nullopt;
    }
    if (dt.timeSpec() == Qt::LocalTime) {
        dt.setTimeZone(QTimeZone(QTimeZone::UTC));
    }
    return dt.toUTC();
}

QHttpServerResponse HandleBatchIdMap(Database& db, const QHttpServerRequest& req,
                                     bool fromOnlineId) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Request body must be a JSON array of ids"));
    }
    const QJsonArray ids = doc.array();
    if (ids.isEmpty() || ids.size() > kMaxBatchIds) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_NUM_ELEMS,
                         QStringLiteral("Request body must list 1-%1 ids").arg(kMaxBatchIds));
    }

    QJsonArray out;
    for (const QJsonValue& v : ids) {
        const QString entry = v.isDouble() ? QString::number(v.toInteger()) : v.toString();
        std::optional<int64_t> userId;
        if (fromOnlineId) {
            if (IsValidOnlineId(entry))
                userId = db.GetUserId(entry);
        } else {
            bool numeric = false;
            const qlonglong asId = entry.toLongLong(&numeric);
            if (numeric && asId != 0 && db.GetUsername(asId).has_value())
                userId = asId;
        }
        if (!userId.has_value())
            continue;
        const auto onlineId = db.GetUsername(*userId);
        if (!onlineId.has_value())
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("accountId"), QString::number(*userId));
        o.insert(QStringLiteral("onlineId"), *onlineId);
        out.append(o);
    }
    qInfo() << "WebAPI:" << (fromOnlineId ? "onlineId2accountId" : "accountId2onlineId") << "batch"
            << ids.size() << "->" << out.size() << "mapped";
    return QHttpServerResponse{"application/json",
                               QJsonDocument(out).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

void RegisterIdentityRoutes(QHttpServer& http, Database& db) {
    // POST /v2/accounts/map/onlineId2accountId -- batch online ID -> account ID.
    http.route("/v2/accounts/map/onlineId2accountId", QHttpServerRequest::Method::Post,
               [&db](const QHttpServerRequest& req) { return HandleBatchIdMap(db, req, true); });

    // POST /v2/accounts/map/accountId2onlineId -- batch account ID -> online ID.
    http.route("/v2/accounts/map/accountId2onlineId", QHttpServerRequest::Method::Post,
               [&db](const QHttpServerRequest& req) { return HandleBatchIdMap(db, req, false); });

    // GET /v2/accounts/map/onlineId2accountId/<online_id>[?asOf=<RFC 3339 timestamp>]
    http.route(
        "/v2/accounts/map/onlineId2accountId/<arg>",
        [&db](const QString& onlineId, const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {QStringLiteral("asOf")};
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value()) {
                return std::move(auth.errorResponse);
            }

            if (!IsValidOnlineId(onlineId)) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                    QStringLiteral("Invalid parameter in request path (parameter: 'online_id')"));
            }

            const QUrlQuery query(req.url());
            const bool hasAsOf = query.hasQueryItem(QStringLiteral("asOf"));
            QDateTime asOf;
            if (hasAsOf) {
                const auto parsed =
                    ParseAsOf(query.queryItemValue(QStringLiteral("asOf"), QUrl::FullyDecoded));
                if (!parsed.has_value()) {
                    return JsonError(
                        QHttpServerResponse::StatusCode::BadRequest, UP_INVALID_QUERY_PARAM,
                        QStringLiteral("Invalid parameter in query string (parameter: 'asOf')"));
                }
                asOf = *parsed;
            }

            const auto userId = db.GetUserId(onlineId);
            if (!userId.has_value()) {
                return JsonError(QHttpServerResponse::StatusCode::NotFound,
                                 WEBAPI_RESOURCE_NOT_FOUND,
                                 QStringLiteral("The online ID has never been associated with an "
                                                "account (online_id: '%1')")
                                     .arg(onlineId));
            }
            const QString canonicalId = db.GetUsername(*userId).value_or(onlineId);
            if (hasAsOf) {
                const auto created = db.GetAccountCreationTime(*userId);
                if (created.has_value() && asOf.toSecsSinceEpoch() < *created) {
                    return JsonError(
                        QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                        QStringLiteral("The online ID was not associated with an account at the "
                                       "requested time (online_id: '%1')")
                            .arg(onlineId));
                }
            }

            QJsonObject body;
            body.insert(QStringLiteral("accountId"), QString::number(*userId));
            body.insert(QStringLiteral("isCurrent"), true);
            if (!hasAsOf) {
                body.insert(QStringLiteral("isRecycled"), false);
            }
            body.insert(QStringLiteral("npId"), EncodeNpId(canonicalId));
            body.insert(QStringLiteral("onlineId"), canonicalId);

            if (hasAsOf) {
                qInfo() << "WebAPI: onlineId2accountId" << canonicalId << "asOf"
                        << asOf.toString(Qt::ISODateWithMs) << "->" << *userId;
            } else {
                qInfo() << "WebAPI: onlineId2accountId" << canonicalId << "->" << *userId;
            }
            return JsonOk(body);
        });
}

} // namespace WebApiRoutes
