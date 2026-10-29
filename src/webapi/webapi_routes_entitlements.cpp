// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webapi_routes_entitlements.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QReadLocker>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include "client_session.h"
#include "database.h"
#include "entitlement_db.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

// Entitlements Web API error codes.
constexpr quint32 ENT_INVALID_REQUEST = 3161857;
constexpr quint32 ENT_INTERNAL_ERROR = 3161858;
constexpr quint32 ENT_INVALID_PARAMETER = 3161861;
constexpr quint32 ENT_INVALID_JSON = 3161912;
constexpr quint32 ENT_NOT_FOUND = 3161955;
constexpr quint32 ENT_NOT_AUTHORIZED = 3161956;

constexpr int kDefaultPageSize = 20;
constexpr int kMaxPageSize = 1000;

bool IsCaller(const QString& userKey, const WebApiAuth::AuthResult& auth) {
    return userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
           userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
           userKey == QString::number(*auth.userId);
}

QString CallerTitleId(SharedState& shared, int64_t userId) {
    QReadLocker lk(&shared.clientsLock);
    auto it = shared.clients.constFind(userId);
    if (it != shared.clients.constEnd() && !it->npTitleId.isEmpty())
        return it->npTitleId;
    return shared.lastLoginTitleId.value(userId);
}

QString Rfc3339(int64_t secs) {
    return QDateTime::fromSecsSinceEpoch(secs).toUTC().toString(
        QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
}

QJsonObject ToJson(const Entitlement& e, int64_t now, bool withActiveFlag) {
    QJsonObject o;
    o.insert(QStringLiteral("active_date"), Rfc3339(e.activeDate));
    if (withActiveFlag)
        o.insert(QStringLiteral("active_flag"), e.IsActive(now));
    o.insert(QStringLiteral("entitlement_type"), e.type);
    o.insert(QStringLiteral("id"), e.label);
    if (e.inactiveDate)
        o.insert(QStringLiteral("inactive_date"), Rfc3339(*e.inactiveDate));
    o.insert(QStringLiteral("is_consumable"), e.consumable);
    o.insert(QStringLiteral("use_count"), static_cast<qint64>(e.useCount));
    o.insert(QStringLiteral("use_limit"), static_cast<qint64>(e.useLimit));
    return o;
}

QHttpServerResponse Error(QHttpServerResponse::StatusCode status, quint32 code,
                          const QString& message) {
    return JsonError(status, code, message);
}

QHttpServerResponse InvalidParameter(const QString& name) {
    return Error(QHttpServerResponse::StatusCode::BadRequest, ENT_INVALID_PARAMETER,
                 QStringLiteral("Invalid Parameter (%1)").arg(name));
}

QHttpServerResponse NotFound() {
    return Error(QHttpServerResponse::StatusCode::NotFound, ENT_NOT_FOUND,
                 QStringLiteral("Entitlement not found."));
}

bool ParseInt(const QUrlQuery& query, const QString& name, int fallback, int min, int max,
              int& out) {
    if (!query.hasQueryItem(name)) {
        out = fallback;
        return true;
    }
    bool ok = false;
    out = query.queryItemValue(name).toInt(&ok);
    return ok && out >= min && out <= max;
}

QStringList SplitValues(const QUrlQuery& query, const QString& name) {
    QStringList out;
    for (const QString& v : query.allQueryItemValues(name, QUrl::FullyDecoded))
        out << v.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString& s : out)
        s = s.trimmed();
    out.removeAll(QString());
    return out;
}

} // namespace

void RegisterEntitlementRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // GET /v1/users/me/entitlements
    //   entitlement_type=service|unified (repeatable; required unless id is given)
    //   [id=A,B] [service_label=n] [fields=active_flag] [sort=active_date]
    //   [direction=asc|desc] [start=<page index>] [size=<page size, default 20>]
    http.route(
        "/v1/users/<arg>/entitlements", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {
                QStringLiteral("entitlement_type"),
                QStringLiteral("id"),
                QStringLiteral("service_label"),
                QStringLiteral("fields"),
                QStringLiteral("sort"),
                QStringLiteral("direction"),
                QStringLiteral("start"),
                QStringLiteral("size"),
            };
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (!IsCaller(userKey, auth))
                return Error(QHttpServerResponse::StatusCode::Forbidden, ENT_NOT_AUTHORIZED,
                             QStringLiteral("Not authorized"));

            const QUrlQuery query(req.url());
            const QStringList types = SplitValues(query, QStringLiteral("entitlement_type"));
            const QStringList ids = SplitValues(query, QStringLiteral("id"));
            if (types.isEmpty() && ids.isEmpty())
                return InvalidParameter(QStringLiteral("entitlement_type"));
            for (const QString& t : types) {
                if (t != QStringLiteral("service") && t != QStringLiteral("unified"))
                    return InvalidParameter(QStringLiteral("entitlement_type"));
            }
            const QString sort = query.queryItemValue(QStringLiteral("sort"));
            if (query.hasQueryItem(QStringLiteral("sort")) && sort != QStringLiteral("active_date"))
                return InvalidParameter(QStringLiteral("sort"));
            const QString direction = query.queryItemValue(QStringLiteral("direction"));
            if (query.hasQueryItem(QStringLiteral("direction")) &&
                direction != QStringLiteral("asc") && direction != QStringLiteral("desc"))
                return InvalidParameter(QStringLiteral("direction"));
            int serviceLabel = 0, start = 0, size = 0;
            if (!ParseInt(query, QStringLiteral("service_label"), 0, 0, 0x7fffffff, serviceLabel))
                return InvalidParameter(QStringLiteral("service_label"));
            if (!ParseInt(query, QStringLiteral("start"), 0, 0, 0x7fffffff, start))
                return InvalidParameter(QStringLiteral("start"));
            if (!ParseInt(query, QStringLiteral("size"), kDefaultPageSize, 1, kMaxPageSize, size))
                return InvalidParameter(QStringLiteral("size"));
            const bool withActiveFlag = SplitValues(query, QStringLiteral("fields"))
                                            .contains(QStringLiteral("active_flag"));

            const QString titleId = CallerTitleId(shared, *auth.userId);
            EntitlementDb edb(db.Conn());
            QVector<Entitlement> rows = edb.ListVisible(*auth.userId, titleId, serviceLabel);
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&](const Entitlement& e) {
                                          return (!types.isEmpty() && !types.contains(e.type)) ||
                                                 (!ids.isEmpty() && !ids.contains(e.label));
                                      }),
                       rows.end());
            if (direction == QStringLiteral("desc"))
                std::reverse(rows.begin(), rows.end());
            const qint64 total = rows.size();
            const qint64 first = std::min<qint64>(static_cast<qint64>(start) * size, total);
            const qint64 last = std::min<qint64>(first + size, total);
            const int64_t now = QDateTime::currentSecsSinceEpoch();
            QJsonArray list;
            for (qint64 i = first; i < last; ++i)
                list.append(ToJson(rows[static_cast<int>(i)], now, withActiveFlag));

            QJsonObject body;
            body.insert(QStringLiteral("entitlements"), list);
            body.insert(QStringLiteral("size"), list.size());
            body.insert(QStringLiteral("start"), start);
            body.insert(QStringLiteral("total_results"), total);
            qInfo() << "WebAPI: entitlements for" << auth.npid << "title" << titleId << "label"
                    << serviceLabel << "types" << types << "ids" << ids << "->" << list.size()
                    << "of" << total;
            return JsonOk(body);
        });

    // GET /v1/users/me/entitlements/<entitlementLabel>[?service_label=n][&fields=active_flag]
    http.route(
        "/v1/users/<arg>/entitlements/<arg>", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey, const QString& label,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {QStringLiteral("service_label"),
                                                 QStringLiteral("fields")};
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (!IsCaller(userKey, auth))
                return Error(QHttpServerResponse::StatusCode::Forbidden, ENT_NOT_AUTHORIZED,
                             QStringLiteral("Not authorized"));

            const QUrlQuery query(req.url());
            int serviceLabel = 0;
            if (!ParseInt(query, QStringLiteral("service_label"), 0, 0, 0x7fffffff, serviceLabel))
                return InvalidParameter(QStringLiteral("service_label"));
            const bool withActiveFlag = SplitValues(query, QStringLiteral("fields"))
                                            .contains(QStringLiteral("active_flag"));

            const QString titleId = CallerTitleId(shared, *auth.userId);
            EntitlementDb edb(db.Conn());
            const auto e = edb.GetVisible(*auth.userId, titleId, serviceLabel, label);
            if (!e) {
                qInfo() << "WebAPI: entitlement" << label << "for" << auth.npid << "title"
                        << titleId << "-> not owned";
                return NotFound();
            }
            qInfo() << "WebAPI: entitlement" << label << "for" << auth.npid << "title" << titleId
                    << "-> use_limit" << e->useLimit;
            return JsonOk(ToJson(*e, QDateTime::currentSecsSinceEpoch(), withActiveFlag));
        });

    // PUT /v1/users/me/entitlements/<entitlementLabel>[?service_label=n]  {"use_count": n}
    // Consumes n uses: use_count += n, use_limit -= n. Replies {"use_limit": <remaining>}.
    http.route(
        "/v1/users/<arg>/entitlements/<arg>", QHttpServerRequest::Method::Put,
        [&db, &shared](const QString& userKey, const QString& label,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            static const QSet<QString> kKnown = {QStringLiteral("service_label")};
            LogUnsupportedQueryParams(req, kKnown);

            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (!IsCaller(userKey, auth))
                return Error(QHttpServerResponse::StatusCode::Forbidden, ENT_NOT_AUTHORIZED,
                             QStringLiteral("Not authorized"));

            const QUrlQuery query(req.url());
            int serviceLabel = 0;
            if (!ParseInt(query, QStringLiteral("service_label"), 0, 0, 0x7fffffff, serviceLabel))
                return InvalidParameter(QStringLiteral("service_label"));

            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return Error(QHttpServerResponse::StatusCode::BadRequest, ENT_INVALID_JSON,
                             QStringLiteral("Invalid JSON"));
            const QJsonValue v = doc.object().value(QStringLiteral("use_count"));
            const double requested = v.toDouble();
            if (!v.isDouble() || requested < 1 || requested > 0x7fffffff ||
                requested != static_cast<double>(static_cast<qint64>(requested)))
                return InvalidParameter(QStringLiteral("use_count"));
            const int64_t count = static_cast<int64_t>(requested);

            const QString titleId = CallerTitleId(shared, *auth.userId);
            EntitlementDb edb(db.Conn());
            ConsumeError cerr = ConsumeError::Internal;
            const auto remaining = edb.Consume(*auth.userId, titleId, serviceLabel, label, count,
                                               QDateTime::currentSecsSinceEpoch(), cerr);
            if (!remaining) {
                auto fail = [&](const char* why) {
                    qInfo() << "WebAPI: consume" << label << "x" << count << "for" << auth.npid
                            << "title" << titleId << "->" << why;
                };
                switch (cerr) {
                case ConsumeError::NotFound:
                    fail("not owned");
                    return NotFound();
                case ConsumeError::NotConsumable:
                    fail("not consumable");
                    return Error(QHttpServerResponse::StatusCode::BadRequest, ENT_INVALID_REQUEST,
                                 QStringLiteral("Entitlement is not consumable."));
                case ConsumeError::Inactive:
                    fail("inactive");
                    return Error(QHttpServerResponse::StatusCode::BadRequest, ENT_INVALID_REQUEST,
                                 QStringLiteral("Entitlement is not active."));
                case ConsumeError::InsufficientUses:
                    fail("not enough uses left");
                    return Error(QHttpServerResponse::StatusCode::BadRequest, ENT_INVALID_REQUEST,
                                 QStringLiteral("Not enough uses remaining."));
                case ConsumeError::Internal:
                    break;
                }
                fail("database error");
                return Error(QHttpServerResponse::StatusCode::InternalServerError,
                             ENT_INTERNAL_ERROR, QStringLiteral("Internal server error"));
            }

            qInfo() << "WebAPI: consume" << label << "x" << count << "for" << auth.npid << "title"
                    << titleId << "-> use_limit" << *remaining;
            QJsonObject body;
            body.insert(QStringLiteral("use_limit"), static_cast<qint64>(*remaining));
            return JsonOk(body);
        });
}

} // namespace WebApiRoutes
