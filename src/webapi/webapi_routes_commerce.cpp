// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_commerce.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QReadLocker>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include "client_session.h"
#include "commerce_db.h"
#include "database.h"
#include "entitlement_db.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"
#include "webapi_routes_entitlements.h"

// Commerce Web API, GET Container.
//
// shadNet has no PlayStation Store catalog: no names, prices or images. Every product label a
// game asks about is answered with a minimal but complete product container (the label as its
// name, one free SKU that can't be bought), so in-game stores and DLC menus load instead of
// failing. What the response does carry faithfully is ownership: the SKU annotation comes from
// the account's entitlements (RED_BAG owned, BLUE_BAG owned consumable, NONE otherwise), which is
// what games check to decide whether DLC is unlocked or "buy" should be offered. Requested labels
// are recorded per title (commerce_db.h) so operators can see which ones to grant.

namespace WebApiRoutes {

namespace {

// Commerce Web API error codes.
constexpr quint32 COM_INVALID_PARAMETER = 3165957;
constexpr quint32 COM_NOT_AUTHORIZED = 3166052;

constexpr qint64 kAnnotationNone = 0;
constexpr qint64 kAnnotationRedBag = 0x80000000LL;  // purchased, can't be purchased again
constexpr qint64 kAnnotationBlueBag = 0x40000000LL; // purchased, can be purchased again

constexpr int kDefaultPageSize = 20;
constexpr int kMaxPageSize = 1000;

bool IsCaller(const QString& userKey, const WebApiAuth::AuthResult& auth) {
    return userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
           userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
           userKey == QString::number(*auth.userId);
}

QHttpServerResponse JsonArrayOk(const QJsonArray& array) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(array).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

QHttpServerResponse InvalidParameter(const QString& name) {
    return JsonError(QHttpServerResponse::StatusCode::BadRequest, COM_INVALID_PARAMETER,
                     QStringLiteral("Invalid Parameter (%1)").arg(name));
}

struct Options {
    int serviceLabel = 0;
    int start = 0; // page index
    int size = kDefaultPageSize;
    QString sort; // name | price | release_date | ""
    bool descending = false;
    bool discounts = false;
    bool useFree = false;
    bool useCurrencySymbol = false;
};

bool ParseBool(const QUrlQuery& query, const QString& name, bool& out) {
    if (!query.hasQueryItem(name))
        return true;
    const QString v = query.queryItemValue(name);
    if (v == QLatin1String("true"))
        out = true;
    else if (v == QLatin1String("false"))
        out = false;
    else
        return false;
    return true;
}

bool ParseInt(const QUrlQuery& query, const QString& name, int min, int max, int& out) {
    if (!query.hasQueryItem(name))
        return true;
    bool ok = false;
    out = query.queryItemValue(name).toInt(&ok);
    return ok && out >= min && out <= max;
}

std::optional<QString> ParseOptions(const QUrlQuery& query, Options& o) {
    // The reference accepts both spellings.
    for (const QString& name : {QStringLiteral("serviceLabel"), QStringLiteral("service_label")}) {
        if (!ParseInt(query, name, 0, 0x7fffffff, o.serviceLabel))
            return name;
    }
    if (!ParseInt(query, QStringLiteral("start"), 0, 0x7fffffff, o.start))
        return QStringLiteral("start");
    if (!ParseInt(query, QStringLiteral("size"), 1, kMaxPageSize, o.size))
        return QStringLiteral("size");
    if (query.hasQueryItem(QStringLiteral("sort"))) {
        o.sort = query.queryItemValue(QStringLiteral("sort"));
        if (o.sort != QLatin1String("name") && o.sort != QLatin1String("price") &&
            o.sort != QLatin1String("release_date"))
            return QStringLiteral("sort");
    }
    if (query.hasQueryItem(QStringLiteral("direction"))) {
        const QString d = query.queryItemValue(QStringLiteral("direction"));
        if (d != QLatin1String("asc") && d != QLatin1String("desc"))
            return QStringLiteral("direction");
        o.descending = d == QLatin1String("desc");
    }
    if (query.hasQueryItem(QStringLiteral("flag"))) {
        if (query.queryItemValue(QStringLiteral("flag")) != QLatin1String("discounts"))
            return QStringLiteral("flag");
        o.discounts = true;
    }
    if (!ParseBool(query, QStringLiteral("useFree"), o.useFree))
        return QStringLiteral("useFree");
    if (!ParseBool(query, QStringLiteral("useCurrencySymbol"), o.useCurrencySymbol))
        return QStringLiteral("useCurrencySymbol");
    bool keepHtml = false; // descriptions carry no HTML, so this only needs validating
    if (!ParseBool(query, QStringLiteral("keepHtmlTag"), keepHtml))
        return QStringLiteral("keepHtmlTag");
    return std::nullopt;
}

QString ProductId(const QString& titleId, int serviceLabel, const QString& label) {
    return QStringLiteral("XX0000-%1_%2-%3")
        .arg(titleId.isEmpty() ? QStringLiteral("XXXX00000") : titleId)
        .arg(serviceLabel, 2, 10, QLatin1Char('0'))
        .arg(label);
}

QString DisplayPrice(const Options& o) {
    if (o.useFree)
        return QStringLiteral("Free");
    return o.useCurrencySymbol ? QStringLiteral("0.00USD") : QStringLiteral("0.00");
}

QString ContainerUrl(const QHttpServerRequest& req, const QString& label) {
    QUrl url = req.url();
    url.setQuery(QString());
    url.setPath(QStringLiteral("/v1/users/me/container/%1").arg(label));
    return url.toString();
}

QJsonObject ProductContainer(const QString& label, const QString& titleId, const Options& o,
                             const std::optional<Entitlement>& owned, int64_t now) {
    const QString productId = ProductId(titleId, o.serviceLabel, label);

    qint64 annotation = kAnnotationNone;
    QString annotationName = QStringLiteral("NONE");
    qint64 usesLeft = 0;
    if (owned && owned->IsActive(now)) {
        if (owned->consumable) {
            annotation = kAnnotationBlueBag;
            annotationName = QStringLiteral("BLUE_BAG");
            usesLeft = owned->useLimit;
        } else {
            annotation = kAnnotationRedBag;
            annotationName = QStringLiteral("RED_BAG");
        }
    }

    QJsonObject sku;
    sku.insert(QStringLiteral("annotation"), annotation);
    sku.insert(QStringLiteral("annotation_name"), annotationName);
    sku.insert(QStringLiteral("count_until_expiration"), usesLeft);
    sku.insert(QStringLiteral("display_price"), DisplayPrice(o));
    sku.insert(QStringLiteral("downloadable"), false);
    sku.insert(QStringLiteral("id"), productId + QStringLiteral("-U001"));
    if (o.discounts)
        sku.insert(QStringLiteral("is_plus_price"), false); // no discounts to report
    // There is no store to buy from, so nothing is offered for sale.
    sku.insert(QStringLiteral("is_purchaseable"), false);
    sku.insert(QStringLiteral("label"), QStringLiteral("U001"));
    sku.insert(QStringLiteral("name"), label);
    sku.insert(QStringLiteral("platforms"), QJsonArray{0});
    sku.insert(QStringLiteral("price"), 0);
    sku.insert(QStringLiteral("product_id"), productId);
    sku.insert(QStringLiteral("sku_type"), 0);
    sku.insert(QStringLiteral("skuAvailabilityOverrideFlag"), false);
    sku.insert(QStringLiteral("type"), QStringLiteral("standard"));
    sku.insert(QStringLiteral("use_count"), 0);

    QJsonObject starRating;
    starRating.insert(QStringLiteral("count"), QJsonValue());
    starRating.insert(QStringLiteral("score"), QStringLiteral("0.00"));
    starRating.insert(QStringLiteral("total"), QStringLiteral("0"));

    QJsonObject c;
    c.insert(QStringLiteral("age_limit"), 0);
    c.insert(QStringLiteral("container_type"), QStringLiteral("product"));
    c.insert(QStringLiteral("content_descriptors"), QJsonArray());
    c.insert(QStringLiteral("content_rating"), QJsonObject());
    c.insert(QStringLiteral("content_type"), QStringLiteral("1"));
    c.insert(QStringLiteral("images"), QJsonArray());
    c.insert(QStringLiteral("label"), label);
    c.insert(QStringLiteral("links"), QJsonArray());
    c.insert(QStringLiteral("long_desc"), QString());
    c.insert(QStringLiteral("name"), label);
    c.insert(QStringLiteral("provider_name"), QString());
    c.insert(QStringLiteral("restricted"), false);
    c.insert(QStringLiteral("size"), 0);
    c.insert(QStringLiteral("sku_links"), QJsonArray());
    c.insert(QStringLiteral("skus"), QJsonArray{sku});
    c.insert(QStringLiteral("star_rating"), starRating);
    c.insert(QStringLiteral("start"), 0);
    c.insert(QStringLiteral("total_results"), 0);
    return c;
}

QString CallerTitleName(SharedState& shared, int64_t userId) {
    QReadLocker lk(&shared.clientsLock);
    auto it = shared.clients.constFind(userId);
    return it != shared.clients.constEnd() ? it->titleName : QString();
}

} // namespace

void RegisterCommerceRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    static const QSet<QString> kKnown = {
        QStringLiteral("direction"),     QStringLiteral("flag"),
        QStringLiteral("keepHtmlTag"),   QStringLiteral("serviceLabel"),
        QStringLiteral("service_label"), QStringLiteral("size"),
        QStringLiteral("sort"),          QStringLiteral("start"),
        QStringLiteral("useFree"),       QStringLiteral("useCurrencySymbol"),
    };

    // GET /v1/users/me/container/<LABEL1:LABEL2:...>
    // One product container per requested label, in request order (duplicates once).
    http.route("/v1/users/<arg>/container/<arg>", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& userKey, const QString& labelList,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   LogUnsupportedQueryParams(req, kKnown);
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value())
                       return std::move(auth.errorResponse);
                   if (!IsCaller(userKey, auth))
                       return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                        COM_NOT_AUTHORIZED, QStringLiteral("Not authorized"));

                   Options o;
                   if (const auto bad = ParseOptions(QUrlQuery(req.url()), o))
                       return InvalidParameter(*bad);

                   static const QRegularExpression labelRe(
                       QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_]{1,32}")));
                   QStringList labels;
                   for (const QString& raw : labelList.split(QLatin1Char(':'))) {
                       const QString label = raw.trimmed();
                       if (!labelRe.match(label).hasMatch())
                           return InvalidParameter(QStringLiteral("label"));
                       if (!labels.contains(label))
                           labels << label;
                   }

                   const QString titleId = CallerTitleId(shared, *auth.userId);
                   const int64_t now = QDateTime::currentSecsSinceEpoch();
                   CommerceDb(db.Conn()).RecordRequested(titleId, o.serviceLabel, labels, now);

                   // Ownership for every requested label in one query.
                   QHash<QString, Entitlement> owned;
                   for (const Entitlement& e :
                        EntitlementDb(db.Conn()).ListVisible(*auth.userId, titleId, o.serviceLabel))
                       owned.insert(e.label, e);

                   QJsonArray containers;
                   int ownedCount = 0;
                   for (const QString& label : labels) {
                       std::optional<Entitlement> e;
                       if (auto it = owned.constFind(label); it != owned.constEnd()) {
                           e = *it;
                           ownedCount += it->IsActive(now) ? 1 : 0;
                       }
                       containers.append(ProductContainer(label, titleId, o, e, now));
                   }
                   qInfo() << "WebAPI: container for" << auth.npid << "title" << titleId << "-"
                           << labels.size() << "product(s)," << ownedCount << "owned";
                   return JsonArrayOk(containers);
               });

    // GET /v1/users/me/container  — the title's top category. Its product links are the labels
    // this title has asked about plus the ones the account has entitlements for.
    http.route(
        "/v1/users/<arg>/container", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            LogUnsupportedQueryParams(req, kKnown);
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (!IsCaller(userKey, auth))
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, COM_NOT_AUTHORIZED,
                                 QStringLiteral("Not authorized"));

            Options o;
            if (const auto bad = ParseOptions(QUrlQuery(req.url()), o))
                return InvalidParameter(*bad);

            const QString titleId = CallerTitleId(shared, *auth.userId);
            QStringList labels;
            for (const CommerceLabel& l : CommerceDb(db.Conn()).ForTitle(titleId, o.serviceLabel))
                labels << l.label;
            for (const Entitlement& e :
                 EntitlementDb(db.Conn()).ListVisible(*auth.userId, titleId, o.serviceLabel))
                if (!labels.contains(e.label))
                    labels << e.label;
            std::sort(labels.begin(), labels.end());
            if (o.descending)
                std::reverse(labels.begin(), labels.end());

            const qint64 total = labels.size();
            const qint64 first = std::min<qint64>(static_cast<qint64>(o.start) * o.size, total);
            const qint64 last = std::min<qint64>(first + o.size, total);
            QJsonArray links;
            for (qint64 i = first; i < last; ++i) {
                const QString& label = labels[static_cast<int>(i)];
                QJsonObject link;
                link.insert(QStringLiteral("container_type"), QStringLiteral("product"));
                link.insert(QStringLiteral("content_type"), QStringLiteral("1"));
                link.insert(QStringLiteral("images"), QJsonArray());
                link.insert(QStringLiteral("label"), label);
                link.insert(QStringLiteral("name"), label);
                link.insert(QStringLiteral("url"), ContainerUrl(req, label));
                links.append(link);
            }

            QString name = CallerTitleName(shared, *auth.userId);
            if (name.isEmpty())
                name = titleId;
            QJsonObject c;
            c.insert(QStringLiteral("age_limit"), 0);
            c.insert(QStringLiteral("container_type"), QStringLiteral("category"));
            c.insert(QStringLiteral("content_rating"), QJsonObject());
            c.insert(QStringLiteral("images"), QJsonArray());
            c.insert(QStringLiteral("label"), QString());
            c.insert(QStringLiteral("links"), links);
            c.insert(QStringLiteral("long_desc"), name);
            c.insert(QStringLiteral("name"), name);
            c.insert(QStringLiteral("restricted"), false);
            c.insert(QStringLiteral("size"), links.size());
            c.insert(QStringLiteral("sku_links"), QJsonArray());
            c.insert(QStringLiteral("sort"), QStringLiteral("static"));
            c.insert(QStringLiteral("start"), o.start);
            c.insert(QStringLiteral("total_results"), total);
            qInfo() << "WebAPI: top category for" << auth.npid << "title" << titleId << "-"
                    << links.size() << "of" << total << "product(s)";
            return JsonArrayOk(QJsonArray{c});
        });
}

} // namespace WebApiRoutes
