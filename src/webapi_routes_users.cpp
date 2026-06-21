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
#include <QStringList>
#include <QUrlQuery>
#include <QDateTime>
#include <QUuid>
#include <algorithm>

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
// NP Service Label query param (service_label=n); default 0.
int ParseServiceLabel(const QHttpServerRequest& req) {
    const QUrlQuery query(req.url());
    // Commerce uses serviceLabel (camelCase); entitlement uses service_label. Accept both.
    QString v = query.queryItemValue(QStringLiteral("serviceLabel"));
    if (v.isEmpty())
        v = query.queryItemValue(QStringLiteral("service_label"));
    bool ok = false;
    const int n = v.toInt(&ok);
    return (ok && n >= 0) ? n : 0;
}

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

// Documented Entitlements Web API error codes (range 3158016-3162111).
namespace EntErr {
constexpr quint32 InvalidRequest = 3161857;      // Invalid Request
constexpr quint32 InvalidParameter = 3161861;    // Invalid Parameter
constexpr quint32 NotImplementedMethod = 3161863;// invalid HTTP method for this API
constexpr quint32 InvalidJson = 3161912;         // request body was invalid
constexpr quint32 InvalidAccessToken = 3158290;  // invalid/corrupted token
constexpr quint32 RateLimitExceeded = 3161905;   // API rate limit exceeded
constexpr quint32 NotAuthorized = 3161956;       // user/client not authorized
constexpr quint32 InternalError = 3161858;       // internal server error
constexpr quint32 ServiceUnavailable = 3161859;  // server unavailable
constexpr quint32 EntitlementNotFound = 3161955; // invalid entitlement label
}  // namespace EntErr

QHttpServerResponse JsonErrorReply(QHttpServerResponse::StatusCode status, quint32 code,
                                   const QString& message) {
    // Error object per the webapi spec: { code, data (reserved), message (<=128 ASCII,
    // English, debug-only), uuid (unique per error) }, wrapped under "error".
    QJsonObject errorObj;
    errorObj.insert("code", static_cast<qint64>(code));
    errorObj.insert("data", QString());
    errorObj.insert("message", message.left(128));
    errorObj.insert("uuid", QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonObject body;
    body.insert("error", errorObj);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

// 405 for HTTP methods a route does not support.
QHttpServerResponse MethodNotAllowed() {
    return JsonErrorReply(QHttpServerResponse::StatusCode::MethodNotAllowed,
                          EntErr::NotImplementedMethod, QStringLiteral("Method not allowed"));
}

// 204 No Content (presence set/clear).
QHttpServerResponse NoContentReply() {
    return QHttpServerResponse(QHttpServerResponse::StatusCode::NoContent);
}

QHttpServerResponse JsonOkReply(const QJsonObject& body) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

// Some webapi responses (e.g. GET Container) are a top-level JSON array.
QHttpServerResponse JsonOkReply(const QJsonArray& body) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

// Build a commerce container "product" entry. Reverse-engineered (no published schema):
// container_type/label/name/long_desc/provider_name/display_price/price/annotation and a
// single default sku (type "standard", default flag 1). Field names beyond those confirmed
// in title decompiles are best-effort and may need tuning against a live capture.
// Build a Commerce "product" container object with its default SKU, per the Commerce
// Web API skus schema. annotation is the per-user purchase status (default 0x0/"NONE" =
// not purchased); count_until_expiration is owner-only (omitted here). The discount block
// (original_price / display_original_price / is_plus_price) is emitted only when the
// request carries flag=discounts AND a discount actually exists.
QJsonObject BuildProductJson(const ProductRecord& r, bool includeDiscounts) {
    QJsonObject sku;
    sku.insert(QStringLiteral("id"), r.label);
    sku.insert(QStringLiteral("product_id"), r.label);
    sku.insert(QStringLiteral("label"), r.label);
    sku.insert(QStringLiteral("name"), r.name);
    sku.insert(QStringLiteral("type"), QStringLiteral("standard"));
    sku.insert(QStringLiteral("sku_type"), 0);  // 0 = Standard
    sku.insert(QStringLiteral("price"), static_cast<qint64>(r.price));
    sku.insert(QStringLiteral("display_price"), r.displayPrice);
    sku.insert(QStringLiteral("annotation"), 0);                      // 0x0 = not purchased
    sku.insert(QStringLiteral("annotation_name"), QStringLiteral("NONE"));
    sku.insert(QStringLiteral("use_count"), static_cast<qint64>(r.useCount));
    sku.insert(QStringLiteral("is_purchaseable"), true);
    sku.insert(QStringLiteral("downloadable"), false);
    sku.insert(QStringLiteral("skuAvailabilityOverrideFlag"), false);
    // Discount pair: only with flag=discounts and only if a discount exists.
    if (includeDiscounts) {
        sku.insert(QStringLiteral("is_plus_price"), false);
        const qint64 orig = r.originalPrice ? r.originalPrice : r.price;
        if (orig > r.price) {
            sku.insert(QStringLiteral("original_price"), orig);
            sku.insert(QStringLiteral("display_original_price"),
                       r.displayOriginalPrice.isEmpty() ? r.displayPrice
                                                        : r.displayOriginalPrice);
        }
    }
    QJsonArray skus;
    skus.append(sku);

    QJsonObject p;
    p.insert(QStringLiteral("age_limit"), 0);
    p.insert(QStringLiteral("container_type"), QStringLiteral("product"));
    p.insert(QStringLiteral("content_type"), QStringLiteral("1"));  // 1 = Game
    p.insert(QStringLiteral("label"), r.label);
    p.insert(QStringLiteral("name"), r.name);
    p.insert(QStringLiteral("long_desc"), r.longDesc);
    p.insert(QStringLiteral("provider_name"), r.providerName);
    p.insert(QStringLiteral("restricted"), false);
    p.insert(QStringLiteral("content_rating"), QJsonObject{});
    p.insert(QStringLiteral("images"), QJsonArray{});
    p.insert(QStringLiteral("links"), QJsonArray{});
    p.insert(QStringLiteral("skus"), skus);
    p.insert(QStringLiteral("size"), 0);
    p.insert(QStringLiteral("start"), 0);
    p.insert(QStringLiteral("total_results"), 0);
    return p;
}

// Build the entitlement data object per the entitlement webapi spec. Keys:
//   id, entitlement_type, is_consumable, active_date, inactive_date, use_limit, use_count.
// active_flag is optional and only included when the caller passes fields=active_flag; it
// reports whether now falls within [active_date, inactive_date].
QJsonObject BuildEntitlementJson(const EntitlementRecord& r, bool includeActiveFlag = false) {
    QJsonObject e;
    e.insert(QStringLiteral("id"), r.entitlementId);
    e.insert(QStringLiteral("entitlement_type"), r.entitlementType);
    e.insert(QStringLiteral("is_consumable"), r.isConsumable);
    e.insert(QStringLiteral("active_date"), r.activeDate);
    // Omitted for entitlements without a time limit (perpetual), per spec.
    if (!r.inactiveDate.isEmpty()) {
        e.insert(QStringLiteral("inactive_date"), r.inactiveDate);
    }
    // use_limit/use_count are present only for consumable entitlements; a durable
    // (unified/permanent) entitlement omits them entirely.
    if (r.isConsumable) {
        e.insert(QStringLiteral("use_limit"), static_cast<qint64>(r.useLimit));
        e.insert(QStringLiteral("use_count"), static_cast<qint64>(r.useCount));
    }
    if (includeActiveFlag) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QDateTime from = QDateTime::fromString(r.activeDate, Qt::ISODate);
        const QDateTime to = QDateTime::fromString(r.inactiveDate, Qt::ISODate);
        const bool active = (!from.isValid() || now >= from) && (!to.isValid() || now < to);
        e.insert(QStringLiteral("active_flag"), active);
    }
    return e;
}

// True when the request asked for discount fields (flag=discounts).
bool WantsDiscounts(const QHttpServerRequest& req) {
    const QUrlQuery query(req.url());
    return query.queryItemValue(QStringLiteral("flag"))
        .split(QLatin1Char(','))
        .contains(QStringLiteral("discounts"));
}

// True when the request asked for the optional active_flag field (fields=active_flag).
bool WantsActiveFlag(const QHttpServerRequest& req) {
    const QUrlQuery query(req.url());
    return query.queryItemValue(QStringLiteral("fields"))
        .split(QLatin1Char(','))
        .contains(QStringLiteral("active_flag"));
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

// Build the GET Container response: a flat array of product container objects. The
// path carries a colon-separated label list (product or category labels); empty means
// "top category" -> all products. We match a product when its label OR its category
// (container_label) is in the list, and return each as a product object.
QJsonArray BuildContainerArray(Database& db, int serviceLabel, const QString& labelArg,
                               bool discounts) {
    const QStringList labels =
        labelArg.isEmpty() ? QStringList()
                           : labelArg.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    QJsonArray out;
    for (const ProductRecord& pr : db.GetAllProducts(serviceLabel)) {
        if (labels.isEmpty() || labels.contains(pr.label) ||
            labels.contains(pr.containerLabel)) {
            out.append(BuildProductJson(pr, discounts));
        }
    }
    return out;
}

void RegisterUserRoutes(QHttpServer& http, Database& db) {
    // GET /v1/users/<userKey>/friendList
    http.route(
        "/v1/users/<arg>/friendList", [&db](const QString& userKey, const QHttpServerRequest& req) {
            static const QSet<QString> kKnown = {
                QStringLiteral("friendStatus"),
                QStringLiteral("fields"),
                QStringLiteral("limit"),
                QStringLiteral("offset"),
                QStringLiteral("presenceType"),  // requests friends' presence (none yet)
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
            qInfo() << "WebAPI: friendList for" << auth.npid << "->" << rels.friends.size();
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
            qInfo() << "WebAPI: blockList for" << auth.npid << "->" << rels.blocked.size();
            return BuildListResponse(rels.blocked, QStringLiteral("blockList"), ParseLimit(req));
        });

    // PUT|DELETE /v1/users/<userKey>/presence/gameStatus  (in-game presence)
    // Body (PUT): {"gameStatus":"..."[,"localizedGameStatus":[...]][,"gameData":"..."]}
    // Stored per user + serviceLabel; returns 204. DELETE clears it.
    http.route("/v1/users/<arg>/presence/gameStatus",
               [&db](const QString& userKey, const QHttpServerRequest& req) {
                   const auto method = req.method();
                   if (method != QHttpServerRequest::Method::Put &&
                       method != QHttpServerRequest::Method::Delete) {
                       return MethodNotAllowed();
                   }
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
                   const int sl = ParseServiceLabel(req);
                   if (method == QHttpServerRequest::Method::Delete) {
                       db.ClearPresence(*auth.userId, sl);
                       qInfo() << "WebAPI: presence cleared for" << auth.npid;
                       return NoContentReply();
                   }
                   // PUT: lenient parse; gameStatus/gameData are optional strings.
                   QString gameStatus;
                   QString gameData;
                   const QByteArray rawBody = req.body();
                   if (!rawBody.isEmpty()) {
                       const QJsonObject obj = QJsonDocument::fromJson(rawBody).object();
                       gameStatus = obj.value(QStringLiteral("gameStatus")).toString();
                       gameData = obj.value(QStringLiteral("gameData")).toString();
                   }
                   db.SetPresence(*auth.userId, sl, gameStatus, gameData);
                   qInfo() << "WebAPI: presence gameStatus for" << auth.npid << "->" << gameStatus;
                   return NoContentReply();
               });

    // GET /v1/users/<userKey>/container/<label>  (np_commerce2 product container)
    http.route("/v1/users/<arg>/container/<arg>",
               [&db](const QString& userKey, const QString& label,
                     const QHttpServerRequest& req) {
                   if (req.method() != QHttpServerRequest::Method::Get) {
                       return MethodNotAllowed();
                   }
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
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }
                   if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }
                   // Flat array of product container objects for the requested labels.
                   const QJsonArray body =
                       BuildContainerArray(db, ParseServiceLabel(req), label, WantsDiscounts(req));
                   qInfo() << "WebAPI: container" << label << "for" << auth.npid
                           << "->" << body.size() << "products";
                   return JsonOkReply(body);
               });

    // GET /v1/users/<userKey>/container  (no label list -> top category / all products)
    http.route("/v1/users/<arg>/container",
               [&db](const QString& userKey, const QHttpServerRequest& req) {
                   if (req.method() != QHttpServerRequest::Method::Get) {
                       return MethodNotAllowed();
                   }
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value() ||
                       resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }
                   const QJsonArray body =
                       BuildContainerArray(db, ParseServiceLabel(req), QString(), WantsDiscounts(req));
                   qInfo() << "WebAPI: container (top) for" << auth.npid
                           << "->" << body.size() << "products";
                   return JsonOkReply(body);
               });

    // POST /v1/commerce/checkout  (emulated-platform grant)
    // Not an official client webapi: on real hardware the Store/VSH grants entitlements
    // after sceNpCommerceDialog CHECKOUT. shadNet emulates that here. The emulator calls
    // this on dialog OK with the CHECKOUT targets it parsed from SceNpCommerceDialogParam.
    // Body: {"targets":["[SL:]PRODUCT[-SKU]", ...]}  (also accepts ?targets=a,b).
    // Each target grants product.use_count to product.entitlement_id (fallback: product label).
    http.route("/v1/commerce/checkout",
               [&db](const QHttpServerRequest& req) {
                   if (req.method() != QHttpServerRequest::Method::Post) {
                       return MethodNotAllowed();
                   }
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   // Collect targets from JSON body, falling back to a ?targets= CSV.
                   QStringList targets;
                   const QByteArray rawBody = req.body();
                   if (!rawBody.isEmpty()) {
                       QJsonParseError perr{};
                       const QJsonDocument doc = QJsonDocument::fromJson(rawBody, &perr);
                       if (perr.error != QJsonParseError::NoError) {
                           return JsonErrorReply(QHttpServerResponse::StatusCode::BadRequest,
                                                 EntErr::InvalidJson, QStringLiteral("Invalid JSON"));
                       }
                       for (const QJsonValue& v : doc.object().value(QStringLiteral("targets")).toArray()) {
                           targets << v.toString();
                       }
                   }
                   if (targets.isEmpty()) {
                       const QString csv = QUrlQuery(req.url()).queryItemValue(QStringLiteral("targets"));
                       if (!csv.isEmpty())
                           targets = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
                   }
                   if (targets.isEmpty()) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::BadRequest,
                                             EntErr::InvalidParameter, QStringLiteral("No targets"));
                   }
                   const int defaultLabel = ParseServiceLabel(req);
                   QJsonArray granted;
                   for (const QString& raw : targets) {
                       // Parse "[serviceLabel:]PRODUCT[-SKU]".
                       QString t = raw;
                       int sl = defaultLabel;
                       const int colon = t.indexOf(QLatin1Char(':'));
                       if (colon > 0) {
                           bool ok = false;
                           const int n = t.left(colon).toInt(&ok);
                           if (ok) {
                               sl = n;
                               t = t.mid(colon + 1);
                           }
                       }
                       const int dash = t.indexOf(QLatin1Char('-'));
                       const QString productLabel = (dash > 0) ? t.left(dash) : t;
                       const auto prod = db.GetProduct(sl, productLabel);
                       if (!prod.has_value()) {
                           qInfo() << "WebAPI: checkout unknown product" << productLabel
                                   << "(sl" << sl << ") for" << auth.npid;
                           continue;  // skip unknown products; report only what we grant
                       }
                       const QString entId =
                           prod->entitlementId.isEmpty() ? prod->label : prod->entitlementId;
                       const int amount = static_cast<int>(prod->useCount);
                       EntitlementRecord rec;
                       if (!db.AddCounts(*auth.userId, entId, sl, amount, &rec)) {
                           return JsonErrorReply(QHttpServerResponse::StatusCode::InternalServerError,
                                                 EntErr::InternalError, QStringLiteral("Grant failed"));
                       }
                       qInfo() << "WebAPI: checkout granted" << amount << "of" << entId
                               << "for" << auth.npid << "use_limit->" << rec.useLimit;
                       QJsonObject g;
                       g.insert(QStringLiteral("product_label"), prod->label);
                       g.insert(QStringLiteral("entitlement_id"), entId);
                       g.insert(QStringLiteral("granted_count"), static_cast<qint64>(amount));
                       g.insert(QStringLiteral("use_limit"), static_cast<qint64>(rec.useLimit));
                       granted.append(g);
                   }
                   QJsonObject body;
                   body.insert(QStringLiteral("granted"), granted);
                   return JsonOkReply(body);
               });

    // GET /v1/users/<userKey>/entitlements  (owned DLC / service entitlements list)
    http.route("/v1/users/<arg>/entitlements",
               [&db](const QString& userKey, const QHttpServerRequest& req) {
                   if (req.method() != QHttpServerRequest::Method::Get) {
                       return MethodNotAllowed();
                   }
                   static const QSet<QString> kKnown = {
                       QStringLiteral("service_label"),    QStringLiteral("entitlement_type"),
                       QStringLiteral("size"),             QStringLiteral("start"),
                       QStringLiteral("fields"),            QStringLiteral("id"),
                       QStringLiteral("sort"),             QStringLiteral("direction"),
                   };
                   LogUnsupportedQueryParams(req, kKnown);

                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value()) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }
                   if (resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }

                   // Serve this account's entitlements across all titles; each game
                   // filters by recognizing its own reverse-DNS ids. Honors the spec query
                   // params: entitlement_type (repeatable), id (CSV), sort=active_date +
                   // direction, and start/size pagination.
                   const QUrlQuery query(req.url());
                   QList<EntitlementRecord> rows =
                       db.GetEntitlements(*auth.userId, ParseServiceLabel(req));

                   const QStringList typeFilter =
                       query.allQueryItemValues(QStringLiteral("entitlement_type"));
                   if (!typeFilter.isEmpty()) {
                       rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const EntitlementRecord& r) {
                                      return !typeFilter.contains(r.entitlementType);
                                  }), rows.end());
                   }

                   if (query.hasQueryItem(QStringLiteral("id"))) {
                       const QStringList ids =
                           query.queryItemValue(QStringLiteral("id"))
                               .split(QLatin1Char(','), Qt::SkipEmptyParts);
                       const QSet<QString> idSet(ids.begin(), ids.end());
                       rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const EntitlementRecord& r) {
                                      return !idSet.contains(r.entitlementId);
                                  }), rows.end());
                   }

                   if (query.queryItemValue(QStringLiteral("sort")) ==
                       QStringLiteral("active_date")) {
                       const bool desc = query.queryItemValue(QStringLiteral("direction")) ==
                                         QStringLiteral("desc");
                       std::sort(rows.begin(), rows.end(),
                                 [desc](const EntitlementRecord& x, const EntitlementRecord& y) {
                                     return desc ? x.activeDate > y.activeDate
                                                 : x.activeDate < y.activeDate;
                                 });
                   }

                   const int total = static_cast<int>(rows.size());
                   int start = 0;
                   int size = 20;  // spec default page size
                   bool ok = false;
                   if (query.hasQueryItem(QStringLiteral("start"))) {
                       const int v = query.queryItemValue(QStringLiteral("start")).toInt(&ok);
                       if (ok && v >= 0) start = v;
                   }
                   if (query.hasQueryItem(QStringLiteral("size"))) {
                       const int v = query.queryItemValue(QStringLiteral("size")).toInt(&ok);
                       if (ok && v > 0) size = v;
                   }

                   const bool activeFlag = WantsActiveFlag(req);
                   QJsonArray a;
                   for (int i = start; i < total && i < start + size; ++i) {
                       a.append(BuildEntitlementJson(rows[i], activeFlag));
                   }

                   QJsonObject body;
                   body.insert(QStringLiteral("entitlements"), a);
                   body.insert(QStringLiteral("size"), static_cast<int>(a.size()));
                   body.insert(QStringLiteral("start"), start);
                   body.insert(QStringLiteral("total_results"), total);
                   qInfo() << "WebAPI: entitlements for" << auth.npid << "->" << a.size()
                           << "of" << total;
                   return JsonOkReply(body);
               });

    // /v1/users/<userKey>/entitlements/<entitlementId>
    //   GET   -> single entitlement lookup (404 if not owned)
    //   PUT   -> consume uses (body {"use_count":N}); decrements use_limit, increments
    //            use_count, persists. Response is {"use_limit":<remaining>} per spec.
    //            Titles that consume internally (e.g. via NpToolkit) may not use it.
    http.route("/v1/users/<arg>/entitlements/<arg>",
               [&db](const QString& userKey, const QString& entitlementId,
                     const QHttpServerRequest& req) {
                   const auto method = req.method();
                   if (method != QHttpServerRequest::Method::Get &&
                       method != QHttpServerRequest::Method::Put) {
                       return MethodNotAllowed();
                   }
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const auto resolvedNpid = ResolveUserSegment(userKey, auth, db);
                   if (!resolvedNpid.has_value() ||
                       resolvedNpid->compare(auth.npid, Qt::CaseInsensitive) != 0) {
                       return JsonErrorReply(QHttpServerResponse::StatusCode::Forbidden,
                                             EntErr::NotAuthorized, QStringLiteral("Not authorized"));
                   }

                   if (req.method() == QHttpServerRequest::Method::Put) {
                       // Body: {"use_count": N} — number of uses to consume (default 1).
                       const QByteArray rawBody = req.body();
                       QJsonParseError perr{};
                       const QJsonDocument doc = QJsonDocument::fromJson(rawBody, &perr);
                       if (!rawBody.isEmpty() && perr.error != QJsonParseError::NoError) {
                           return JsonErrorReply(QHttpServerResponse::StatusCode::BadRequest,
                                                 EntErr::InvalidJson, QStringLiteral("Invalid JSON"));
                       }
                       const QJsonObject in = doc.object();
                       int count = 1;
                       if (in.contains(QStringLiteral("use_count"))) {
                           count = in.value(QStringLiteral("use_count")).toInt(0);
                       }
                       if (count < 1) {
                           return JsonErrorReply(QHttpServerResponse::StatusCode::BadRequest,
                                                 EntErr::InvalidParameter, QStringLiteral("Invalid Parameter"));
                       }
                       EntitlementRecord rec;
                       switch (db.ConsumeEntitlement(*auth.userId, entitlementId,
                                                     ParseServiceLabel(req), count, &rec)) {
                       case Database::ConsumeResult::Ok: {
                           qInfo() << "WebAPI: consumed" << count << "of" << entitlementId
                                   << "for" << auth.npid << "use_limit->" << rec.useLimit;
                           QJsonObject body;
                           body.insert(QStringLiteral("use_limit"), static_cast<qint64>(rec.useLimit));
                           return JsonOkReply(body);
                       }
                       case Database::ConsumeResult::Exhausted:
                           return JsonErrorReply(QHttpServerResponse::StatusCode::BadRequest,
                                                 EntErr::InvalidParameter, QStringLiteral("Invalid Parameter"));
                       case Database::ConsumeResult::NotFound:
                           return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound,
                                                 EntErr::EntitlementNotFound, QStringLiteral("Entitlement not found"));
                       default:
                           return JsonErrorReply(QHttpServerResponse::StatusCode::InternalServerError,
                                                 EntErr::InternalError, QStringLiteral("Internal server error"));
                       }
                   }

                   // GET
                   if (auto rec = db.GetEntitlement(*auth.userId, entitlementId,
                                                    ParseServiceLabel(req))) {
                       return JsonOkReply(BuildEntitlementJson(*rec, WantsActiveFlag(req)));
                   }
                   qInfo() << "WebAPI: entitlement" << entitlementId
                           << "not owned for" << auth.npid;
                   return JsonErrorReply(QHttpServerResponse::StatusCode::NotFound,
                                         EntErr::EntitlementNotFound, QStringLiteral("Entitlement not found"));
               });
}

} // namespace WebApiRoutes
