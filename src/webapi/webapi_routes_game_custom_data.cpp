// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webapi_routes_game_custom_data.h"

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
#include <QUrl>
#include <QUrlQuery>

#include "client_session.h"
#include "database.h"
#include "game_custom_data_db.h"
#include "webapi_auth.h"
#include "webapi_multipart.h"
#include "webapi_routes_common.h"
#include "webapi_routes_entitlements.h"

// Game Custom Data Web API (sdkGameCustomDataBaseUrl/v1/...):
//   POST /v1/gameCustomData/items                               send (multipart/mixed)
//   GET  /v1/users/<me>/gameCustomData/items                    list received
//   GET  /v1/users/<me>/gameCustomData/items/@items?itemIds=..  message info for several items
//   GET  /v1/users/<me>/gameCustomData/items/<id>/message       message info for one item
//   GET  /v1/users/<me>/gameCustomData/items/<id>/gameData      attached game data
//   PUT  /v1/users/<me>/gameCustomData/items/<id>               {"dataUsedFlag":true}
//   GET  /v1/users/<me>/gameCustomData/items/<id>/thumbnail     shadNet extension: the thumbnail,
//                                                               for the emulator's receive dialog
// Items are only visible to callers playing the same game as the sender (same NP Communication
// ID, else same title serial). Callers are PS4 titles, so an item must list "PS4" in
// availablePlatforms for its game data or URL to be readable.

namespace WebApiRoutes {

namespace {

// Game Custom Data Web API error codes.
constexpr quint32 GCD_BAD_REQUEST = 2117633;
constexpr quint32 GCD_TOO_LARGE_BODY = 2117635;
constexpr quint32 GCD_USER_NOT_FOUND = 2117644;
constexpr quint32 GCD_RESOURCE_NOT_FOUND = 2117645;
constexpr quint32 GCD_ACCESS_DENIED_OWNERSHIP = 2117646;
constexpr quint32 GCD_INVALID_QUERY_PARAM = 2117889;
constexpr quint32 GCD_INVALID_URL_PARAM = 2117892;
constexpr quint32 GCD_QUERY_PARAM_REQUIRED = 2117893;
constexpr quint32 GCD_INVALID_HEADER = 2117952;
constexpr quint32 GCD_INVALID_BODY = 2118016;
constexpr quint32 GCD_NULL_NOT_ALLOWED = 2118017;
constexpr quint32 GCD_INVALID_BODY_PARAM = 2118018;
constexpr quint32 GCD_INVALID_NUM_ELEMENTS = 2118019;
constexpr quint32 GCD_INVALID_BODY_COMBINATION = 2118020;
constexpr quint32 GCD_BLOCKED_USER = 2118021;
constexpr quint32 GCD_BODY_PARAM_REQUIRED = 2118022;
constexpr quint32 GCD_UNEXPECTED_ERROR = 2118400;
constexpr quint32 GCD_TOO_LARGE_IMAGE = 2118656;
constexpr quint32 GCD_EXPIRED = 2119680;
constexpr quint32 GCD_ALREADY_USED = 2119681;
constexpr quint32 GCD_URL_FLAG = 2119685;

constexpr int kMaxRecipients = 16;
constexpr int kMaxMessageChars = 512;
constexpr int kMaxNameChars = 128;
constexpr int kMaxDescriptionChars = 512;
constexpr int kMaxUrlChars = 1024;
constexpr int kMaxGameData = 1024 * 1024;
constexpr int kMaxThumbnail = 160 * 1024;
constexpr qint64 kMaxExpireMinutes = 2147483647;
constexpr int kMaxListLimit = 100;
constexpr int kMaxItemIds = 20;
const QString kCallerPlatform = QStringLiteral("PS4");

using Status = QHttpServerResponse::StatusCode;

QHttpServerResponse Error(Status status, quint32 code, const QString& message) {
    return JsonError(status, code, message);
}
QHttpServerResponse BodyParam(const QString& name) {
    return Error(Status::BadRequest, GCD_INVALID_BODY_PARAM,
                 QStringLiteral("Invalid parameter in body (parameter: '%1')").arg(name));
}
QHttpServerResponse BodyParamRequired(const QString& name) {
    return Error(Status::BadRequest, GCD_BODY_PARAM_REQUIRED,
                 QStringLiteral("'%1' parameter required in body").arg(name));
}
QHttpServerResponse QueryParam(const QString& name) {
    return Error(Status::BadRequest, GCD_INVALID_QUERY_PARAM,
                 QStringLiteral("Invalid parameter in query string (parameter: '%1')").arg(name));
}
QHttpServerResponse NotFound(int64_t itemId) {
    return Error(Status::NotFound, GCD_RESOURCE_NOT_FOUND,
                 QStringLiteral("Resource not found (itemId='%1')").arg(itemId));
}

bool IsCaller(const QString& userKey, const WebApiAuth::AuthResult& auth) {
    return userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
           userKey.compare(auth.npid, Qt::CaseInsensitive) == 0 ||
           userKey == QString::number(*auth.userId);
}

int CharCount(const QString& s) {
    return static_cast<int>(s.toUcs4().size());
}

QString Rfc3339(int64_t secs) {
    return QDateTime::fromSecsSinceEpoch(secs).toUTC().toString(
        QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
}

GameCustomDataScope CallerScope(SharedState& shared, int64_t userId) {
    GameCustomDataScope scope;
    {
        QReadLocker lk(&shared.usageLock);
        scope.communicationId = shared.usageClientGame.value(userId);
    }
    scope.titleId = CallerTitleId(shared, userId);
    return scope;
}

std::optional<int64_t> ParseItemId(const QString& text) {
    bool ok = false;
    const qlonglong id = text.toLongLong(&ok);
    if (!ok || id <= 0)
        return std::nullopt;
    return id;
}

std::optional<QSet<QString>> ParseFields(const QUrlQuery& query, const QSet<QString>& allowed,
                                         const QSet<QString>& defaults) {
    QSet<QString> out;
    const QStringList fields = query.queryItemValue(QStringLiteral("fields"), QUrl::FullyDecoded)
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (fields.isEmpty())
        return defaults;
    for (const QString& f : fields) {
        if (f == QStringLiteral("@default"))
            out.unite(defaults);
        else if (allowed.contains(f))
            out.insert(f);
        else
            return std::nullopt;
    }
    return out;
}

bool UsableHere(const GameCustomDataItem& item) {
    return item.platforms.contains(kCallerPlatform);
}

QJsonObject ItemJson(const GameCustomDataItem& item, const QSet<QString>& fields, int64_t now,
                     const QString& npLanguage, bool withMessage) {
    QJsonObject o;
    if (fields.contains(QStringLiteral("itemId")))
        o.insert(QStringLiteral("itemId"), static_cast<qint64>(item.itemId));
    if (fields.contains(QStringLiteral("fromUser"))) {
        QJsonObject from;
        from.insert(QStringLiteral("onlineId"), item.fromNpid);
        from.insert(QStringLiteral("accountId"), QString::number(item.fromUserId));
        o.insert(QStringLiteral("fromUser"), from);
    }
    if (fields.contains(QStringLiteral("receivedDate")))
        o.insert(QStringLiteral("receivedDate"), Rfc3339(item.createdAt));
    if (fields.contains(QStringLiteral("dataUsedFlag")))
        o.insert(QStringLiteral("dataUsedFlag"), item.IsUrl() ? false : item.used);
    if (fields.contains(QStringLiteral("attachmentDetail"))) {
        QJsonObject a;
        a.insert(QStringLiteral("type"),
                 item.IsUrl() ? QStringLiteral("url") : QStringLiteral("gameData"));
        // The URL is only handed out while the item is live and usable on this platform.
        if (item.IsUrl() && !item.IsExpired(now) && UsableHere(item))
            a.insert(QStringLiteral("value"), item.url);
        o.insert(QStringLiteral("attachmentDetail"), a);
    }
    if (fields.contains(QStringLiteral("gameDataDetail"))) {
        QJsonObject d;
        d.insert(QStringLiteral("availablePlatforms"), QJsonArray::fromStringList(item.platforms));
        if (item.expiresAt)
            d.insert(QStringLiteral("expiredDate"), Rfc3339(*item.expiresAt));
        if (withMessage) {
            d.insert(QStringLiteral("dataName"),
                     item.localizedNames.value(npLanguage, item.dataName));
            d.insert(QStringLiteral("dataDescription"),
                     item.localizedDescriptions.value(npLanguage, item.dataDescription));
        }
        o.insert(QStringLiteral("gameDataDetail"), d);
    }
    if (withMessage && fields.contains(QStringLiteral("message")))
        o.insert(QStringLiteral("message"), item.message);
    return o;
}

QSet<QString> AcceptedLanguages(const QHttpServerRequest& req) {
    QSet<QString> out;
    for (const QString& part : QString::fromUtf8(req.value("Accept-Language"))
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts))
        out.insert(part.section(QLatin1Char(';'), 0, 0).trimmed());
    out.remove(QString());
    return out;
}

// localizedDataNames / localizedDataDescriptions: [{"npLanguage":..., <key>:...}].
std::optional<QHttpServerResponse> ParseLocalized(const QJsonObject& detail, const QString& member,
                                                  const QString& key, int maxChars,
                                                  const QSet<QString>& accepted,
                                                  QHash<QString, QString>& out) {
    if (!detail.contains(member))
        return std::nullopt;
    const QJsonValue v = detail.value(member);
    if (!v.isArray())
        return BodyParam(member);
    for (const QJsonValue& e : v.toArray()) {
        const QJsonObject o = e.toObject();
        const QString lang = o.value(QStringLiteral("npLanguage")).toString();
        const QJsonValue text = o.value(key);
        if (lang.isEmpty() || !text.isString() || text.toString().isEmpty() ||
            CharCount(text.toString()) > maxChars)
            return BodyParam(member);
        // The language must be one the request declared (when it declared any).
        if (!accepted.isEmpty() && !accepted.contains(lang))
            return BodyParam(member + QStringLiteral(".npLanguage"));
        out.insert(lang, text.toString());
    }
    return std::nullopt;
}

bool IsBlockedBetween(Database& db, int64_t a, int64_t b) {
    const auto blockedBy = [&db](int64_t who, int64_t target) {
        for (const auto& entry : db.GetRelationships(who).blocked)
            if (entry.first == target)
                return true;
        return false;
    };
    return blockedBy(a, b) || blockedBy(b, a);
}

// Game Custom Data Send Event (np:service:gameCustomData, NP service name gameCustomData)
void SendGameCustomDataEvent(SharedState& shared, int64_t fromUserId, const QString& fromNpid,
                             const QString& senderComId,
                             const QVector<QPair<int64_t, QString>>& recipients) {
    QHash<int64_t, QString> recipientComIds;
    {
        QReadLocker ul(&shared.usageLock);
        for (const auto& r : recipients)
            recipientComIds.insert(r.first, shared.usageClientGame.value(r.first));
    }
    QReadLocker lk(&shared.clientsLock);
    for (const auto& [toId, toNpid] : recipients) {
        const QString comId = recipientComIds.value(toId);
        if (!senderComId.isEmpty() && !comId.isEmpty() && comId != senderComId)
            continue;
        const auto it = shared.clients.constFind(toId);
        if (it == shared.clients.constEnd() || !it->send)
            continue;
        QList<QPair<QString, QString>> extd;
        extd.append({QStringLiteral("fromAccountId"), QString::number(fromUserId)});
        extd.append({QStringLiteral("toAccountId"), QString::number(toId)});
        it->send(ClientSession::BuildNotification(
            NotificationType::WebApiPushEvent,
            ClientSession::BuildWebApiPushPayload(QStringLiteral("gameCustomData"), 0,
                                                  QStringLiteral("np:service:gameCustomData"), {},
                                                  fromNpid, toNpid, extd, fromUserId, toId)));
    }
}

// POST /v1/gameCustomData/items
QHttpServerResponse HandleSend(Database& db, SharedState& shared, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value())
        return std::move(auth.errorResponse);

    const QByteArray boundary = ExtractBoundary(NormalizeContentType(req.value("Content-Type")));
    if (boundary.isEmpty())
        return Error(Status::BadRequest, GCD_INVALID_HEADER,
                     QStringLiteral("Invalid header (header: 'Content-Type')"));

    QByteArray messagePart, gameData, thumbnail;
    bool haveMessage = false, haveGameData = false, haveThumbnail = false;
    for (const MultipartPart& p : ParseMultipartMixed(req.body(), boundary)) {
        if (p.contentDescription == "message") {
            messagePart = p.data;
            haveMessage = true;
        } else if (p.contentDescription == "game-data-binary-0") {
            gameData = p.data;
            haveGameData = true;
        } else if (p.contentDescription == "game-data-thumbnail-0") {
            thumbnail = p.data;
            haveThumbnail = true;
        } else {
            qWarning() << "WebAPI: gameCustomData ignoring part" << p.contentDescription;
        }
    }
    if (!haveMessage)
        return BodyParamRequired(QStringLiteral("message"));

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(messagePart, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return Error(Status::BadRequest, GCD_INVALID_BODY, QStringLiteral("Invalid body"));
    const QJsonObject root = doc.object();

    if (!root.contains(QStringLiteral("toAccountIds")))
        return BodyParamRequired(QStringLiteral("toAccountIds"));
    const QJsonValue toValue = root.value(QStringLiteral("toAccountIds"));
    if (toValue.isNull())
        return Error(Status::BadRequest, GCD_NULL_NOT_ALLOWED,
                     QStringLiteral("Null value not allowed (parameter: 'toAccountIds')"));
    const QJsonArray toArray = toValue.toArray();
    if (!toValue.isArray() || toArray.isEmpty() || toArray.size() > kMaxRecipients)
        return Error(Status::BadRequest, GCD_INVALID_NUM_ELEMENTS,
                     QStringLiteral("Invalid number of elements (parameter: 'toAccountIds')"));
    QVector<QPair<int64_t, QString>> recipients;
    for (const QJsonValue& v : toArray) {
        const QString entry = v.isDouble() ? QString::number(v.toInteger()) : v.toString();
        bool numeric = false;
        const qlonglong asId = entry.toLongLong(&numeric);
        std::optional<QString> npid;
        int64_t uid = 0;
        if (numeric && asId > 0 && (npid = db.GetUsername(asId))) {
            uid = asId;
        } else if (const auto id = db.GetUserId(entry)) {
            uid = *id;
            npid = db.GetUsername(uid).value_or(entry);
        }
        if (uid == 0)
            return Error(Status::NotFound, GCD_USER_NOT_FOUND,
                         QStringLiteral("User not found (user: '%1')").arg(entry));
        const bool dup = std::any_of(recipients.cbegin(), recipients.cend(),
                                     [uid](const auto& r) { return r.first == uid; });
        if (!dup)
            recipients.append({uid, *npid});
    }
    for (const auto& r : recipients) {
        if (IsBlockedBetween(db, *auth.userId, r.first))
            return Error(Status::Forbidden, GCD_BLOCKED_USER,
                         QStringLiteral("Blocked user exists (user: '%1')").arg(r.second));
    }

    if (!root.value(QStringLiteral("item")).isObject())
        return BodyParamRequired(QStringLiteral("item"));
    const QJsonObject itemObj = root.value(QStringLiteral("item")).toObject();

    GameCustomDataItem item;
    item.fromUserId = *auth.userId;
    const QJsonValue messageValue = itemObj.value(QStringLiteral("message"));
    if (!messageValue.isUndefined() && !messageValue.isString())
        return BodyParam(QStringLiteral("message"));
    item.message = messageValue.toString();
    if (CharCount(item.message) > kMaxMessageChars)
        return BodyParam(QStringLiteral("message"));

    if (!itemObj.value(QStringLiteral("gameDataDetail")).isObject())
        return BodyParamRequired(QStringLiteral("gameDataDetail"));
    const QJsonObject detail = itemObj.value(QStringLiteral("gameDataDetail")).toObject();
    const QJsonValue name = detail.value(QStringLiteral("dataName"));
    if (!name.isString())
        return BodyParamRequired(QStringLiteral("dataName"));
    item.dataName = name.toString();
    if (item.dataName.isEmpty() || CharCount(item.dataName) > kMaxNameChars)
        return BodyParam(QStringLiteral("dataName"));
    const QJsonValue description = detail.value(QStringLiteral("dataDescription"));
    if (!description.isString())
        return BodyParamRequired(QStringLiteral("dataDescription"));
    item.dataDescription = description.toString();
    if (item.dataDescription.isEmpty() || CharCount(item.dataDescription) > kMaxDescriptionChars)
        return BodyParam(QStringLiteral("dataDescription"));
    const QJsonValue expire = detail.value(QStringLiteral("expireMinutes"));
    if (!expire.isDouble())
        return BodyParamRequired(QStringLiteral("expireMinutes"));
    const double expireMinutes = expire.toDouble();
    if (expireMinutes < 0 || expireMinutes > kMaxExpireMinutes ||
        expireMinutes != static_cast<double>(static_cast<qint64>(expireMinutes)))
        return BodyParam(QStringLiteral("expireMinutes"));
    const QJsonValue platforms = detail.value(QStringLiteral("availablePlatforms"));
    if (!platforms.isArray())
        return BodyParamRequired(QStringLiteral("availablePlatforms"));
    for (const QJsonValue& p : platforms.toArray()) {
        const QString platform = p.toString();
        if (platform != QStringLiteral("PS4") && platform != QStringLiteral("PSVITA"))
            return BodyParam(QStringLiteral("availablePlatforms"));
        if (!item.platforms.contains(platform))
            item.platforms << platform;
    }
    // Data sent from a PS4 must at least be usable on PS4.
    if (!item.platforms.contains(kCallerPlatform))
        return BodyParam(QStringLiteral("availablePlatforms"));

    const QSet<QString> accepted = AcceptedLanguages(req);
    if (auto e =
            ParseLocalized(detail, QStringLiteral("localizedDataNames"), QStringLiteral("dataName"),
                           kMaxNameChars, accepted, item.localizedNames))
        return std::move(*e);
    if (auto e = ParseLocalized(detail, QStringLiteral("localizedDataDescriptions"),
                                QStringLiteral("dataDescription"), kMaxDescriptionChars, accepted,
                                item.localizedDescriptions))
        return std::move(*e);

    bool withItemId = false;
    if (itemObj.contains(QStringLiteral("attachmentDetail"))) {
        const QJsonObject a = itemObj.value(QStringLiteral("attachmentDetail")).toObject();
        const QString type = a.value(QStringLiteral("type")).toString();
        if (type.compare(QStringLiteral("url"), Qt::CaseInsensitive) == 0) {
            item.attachmentType = QStringLiteral("url");
            const QJsonValue value = a.value(QStringLiteral("value"));
            if (!value.isString())
                return BodyParamRequired(QStringLiteral("attachmentDetail.value"));
            item.url = value.toString();
            const QUrl url(item.url, QUrl::StrictMode);
            const bool ascii = std::all_of(item.url.cbegin(), item.url.cend(),
                                           [](QChar c) { return c.unicode() < 0x80; });
            if (item.url.isEmpty() || item.url.size() > kMaxUrlChars || !ascii || !url.isValid() ||
                (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")))
                return BodyParam(QStringLiteral("attachmentDetail.value"));
            const QJsonValue flag = a.value(QStringLiteral("withItemId"));
            if (!flag.isUndefined() && !flag.isBool())
                return BodyParam(QStringLiteral("attachmentDetail.withItemId"));
            withItemId = flag.toBool();
        } else if (type.compare(QStringLiteral("gameData"), Qt::CaseInsensitive) == 0) {
            if (a.contains(QStringLiteral("value")) || a.contains(QStringLiteral("withItemId")))
                return Error(Status::BadRequest, GCD_INVALID_BODY_COMBINATION,
                             QStringLiteral("Invalid parameter combination in body (parameters: "
                                            "'attachmentDetail.type', 'attachmentDetail.value')"));
        } else {
            return BodyParam(QStringLiteral("attachmentDetail.type"));
        }
    }
    if (!item.IsUrl()) {
        if (!haveGameData || gameData.isEmpty())
            return BodyParamRequired(QStringLiteral("game-data-binary-0"));
        if (gameData.size() > kMaxGameData)
            return Error(Status::BadRequest, GCD_TOO_LARGE_BODY, QStringLiteral("Too large body"));
    } else if (haveGameData) {
        qWarning() << "WebAPI: gameCustomData URL item from" << auth.npid
                   << "also carried game data; ignored";
    }
    if (!haveThumbnail || thumbnail.isEmpty())
        return BodyParamRequired(QStringLiteral("game-data-thumbnail-0"));
    if (thumbnail.size() > kMaxThumbnail)
        return Error(Status::BadRequest, GCD_TOO_LARGE_IMAGE,
                     QStringLiteral("Too large image data"));

    const GameCustomDataScope scope = CallerScope(shared, *auth.userId);
    item.communicationId = scope.communicationId;
    item.titleId = scope.titleId;
    item.createdAt = QDateTime::currentSecsSinceEpoch();
    if (expireMinutes > 0)
        item.expiresAt = item.createdAt + static_cast<qint64>(expireMinutes) * 60;

    QVector<int64_t> recipientIds;
    for (const auto& r : recipients)
        recipientIds.append(r.first);
    GameCustomDataDb gdb(db.Conn());
    const auto itemId = gdb.Create(item, withItemId, gameData, thumbnail, recipientIds);
    if (!itemId)
        return Error(Status::InternalServerError, GCD_UNEXPECTED_ERROR,
                     QStringLiteral("Unexpected error"));

    QStringList names;
    for (const auto& r : recipients)
        names << r.second;
    qInfo().noquote() << "WebAPI: gameCustomData item" << *itemId << "from" << auth.npid << "to ["
                      << names.join(QLatin1Char(',')) << "]" << item.attachmentType
                      << (item.IsUrl() ? item.url : QStringLiteral("%1 bytes").arg(gameData.size()))
                      << "comId"
                      << (scope.communicationId.isEmpty() ? QStringLiteral("unknown")
                                                          : scope.communicationId)
                      << "title" << scope.titleId;
    SendGameCustomDataEvent(shared, *auth.userId, auth.npid, scope.communicationId, recipients);
    return QHttpServerResponse{Status::NoContent};
}

std::optional<QHttpServerResponse> CheckCaller(const WebApiAuth::AuthResult& auth,
                                               const QString& userKey) {
    if (!IsCaller(userKey, auth))
        return Error(Status::Forbidden, GCD_ACCESS_DENIED_OWNERSHIP,
                     QStringLiteral("Access denied by resource ownership"));
    return std::nullopt;
}

const QSet<QString> kListFields = {
    QStringLiteral("itemId"),       QStringLiteral("fromUser"),
    QStringLiteral("receivedDate"), QStringLiteral("attachmentDetail"),
    QStringLiteral("dataUsedFlag"), QStringLiteral("gameDataDetail")};
const QSet<QString> kListDefaults = {QStringLiteral("itemId"), QStringLiteral("receivedDate"),
                                     QStringLiteral("dataUsedFlag")};
const QSet<QString> kMessageFields = {QStringLiteral("message"), QStringLiteral("gameDataDetail"),
                                      QStringLiteral("attachmentDetail")};
const QSet<QString> kMessageDefaults = {QStringLiteral("itemId"), QStringLiteral("fromUser"),
                                        QStringLiteral("receivedDate"),
                                        QStringLiteral("dataUsedFlag")};

} // namespace

void RegisterGameCustomDataRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    http.route("/v1/gameCustomData/items", QHttpServerRequest::Method::Post,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSend(db, shared, req);
               });

    // GET /v1/users/me/gameCustomData/items[?fields=..][&limit=1-100][&offset=n]
    http.route("/v1/users/<arg>/gameCustomData/items", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   LogUnsupportedQueryParams(req,
                                             {QStringLiteral("fields"), QStringLiteral("limit"),
                                              QStringLiteral("offset")});
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value())
                       return std::move(auth.errorResponse);
                   if (auto e = CheckCaller(auth, userKey))
                       return std::move(*e);

                   const QUrlQuery query(req.url());
                   const auto fields = ParseFields(query, kListFields, kListDefaults);
                   if (!fields)
                       return QueryParam(QStringLiteral("fields"));
                   bool ok = true;
                   int limit = kMaxListLimit, offset = 0;
                   if (query.hasQueryItem(QStringLiteral("limit"))) {
                       limit = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
                       if (!ok || limit < 1 || limit > kMaxListLimit)
                           return QueryParam(QStringLiteral("limit"));
                   }
                   if (query.hasQueryItem(QStringLiteral("offset"))) {
                       offset = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
                       if (!ok || offset < 0)
                           return QueryParam(QStringLiteral("offset"));
                   }

                   const auto all = GameCustomDataDb(db.Conn()).ListReceived(
                       *auth.userId, CallerScope(shared, *auth.userId));
                   const int64_t now = QDateTime::currentSecsSinceEpoch();
                   QJsonArray items;
                   for (int i = offset; i < all.size() && items.size() < limit; ++i)
                       items.append(ItemJson(all[i], *fields, now, {}, /*withMessage=*/false));
                   QJsonObject body;
                   body.insert(QStringLiteral("items"), items);
                   body.insert(QStringLiteral("size"), items.size());
                   body.insert(QStringLiteral("start"), offset);
                   body.insert(QStringLiteral("totalResults"), all.size());
                   qInfo() << "WebAPI: gameCustomData list for" << auth.npid << "->" << items.size()
                           << "of" << all.size();
                   return JsonOk(body);
               });

    // GET /v1/users/me/gameCustomData/items/@items?itemIds=1,2,3[&fields=..][&npLanguage=..]
    // (No other GET exists on a bare item.)
    http.route(
        "/v1/users/<arg>/gameCustomData/items/<arg>", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey, const QString& which,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            LogUnsupportedQueryParams(req, {QStringLiteral("itemIds"), QStringLiteral("fields"),
                                            QStringLiteral("npLanguage")});
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (auto e = CheckCaller(auth, userKey))
                return std::move(*e);
            if (which != QStringLiteral("@items"))
                return Error(Status::NotFound, GCD_RESOURCE_NOT_FOUND,
                             QStringLiteral("Resource not found (itemId='%1')").arg(which));

            const QUrlQuery query(req.url());
            if (!query.hasQueryItem(QStringLiteral("itemIds")))
                return Error(Status::BadRequest, GCD_QUERY_PARAM_REQUIRED,
                             QStringLiteral("'itemIds' parameter required in query string"));
            const QStringList ids = query.queryItemValue(QStringLiteral("itemIds"))
                                        .split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (ids.isEmpty() || ids.size() > kMaxItemIds)
                return QueryParam(QStringLiteral("itemIds"));
            QSet<int64_t> wanted;
            for (const QString& id : ids) {
                const auto itemId = ParseItemId(id.trimmed());
                if (!itemId)
                    return QueryParam(QStringLiteral("itemIds"));
                wanted.insert(*itemId);
            }
            const auto fields = ParseFields(query, kMessageFields, kMessageDefaults);
            if (!fields)
                return QueryParam(QStringLiteral("fields"));
            const QString npLanguage = query.queryItemValue(QStringLiteral("npLanguage"));

            const int64_t now = QDateTime::currentSecsSinceEpoch();
            QJsonArray items;
            for (const auto& item : GameCustomDataDb(db.Conn()).ListReceived(
                     *auth.userId, CallerScope(shared, *auth.userId))) {
                if (wanted.contains(item.itemId))
                    items.append(ItemJson(item, *fields, now, npLanguage, /*withMessage=*/true));
            }
            QJsonObject body;
            body.insert(QStringLiteral("items"), items);
            return JsonOk(body);
        });

    // GET /v1/users/me/gameCustomData/items/<itemId>/message[?fields=..][&npLanguage=..]
    http.route(
        "/v1/users/<arg>/gameCustomData/items/<arg>/message", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey, const QString& id,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            LogUnsupportedQueryParams(req,
                                      {QStringLiteral("fields"), QStringLiteral("npLanguage")});
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (auto e = CheckCaller(auth, userKey))
                return std::move(*e);
            const auto itemId = ParseItemId(id);
            if (!itemId)
                return Error(Status::BadRequest, GCD_INVALID_URL_PARAM,
                             QStringLiteral("Invalid parameter in URL (parameter: 'itemId')"));
            const QUrlQuery query(req.url());
            const auto fields = ParseFields(query, kMessageFields, kMessageDefaults);
            if (!fields)
                return QueryParam(QStringLiteral("fields"));

            const auto item = GameCustomDataDb(db.Conn()).GetReceived(
                *auth.userId, *itemId, CallerScope(shared, *auth.userId));
            if (!item)
                return NotFound(*itemId);
            QJsonObject body;
            body.insert(QStringLiteral("item"),
                        ItemJson(*item, *fields, QDateTime::currentSecsSinceEpoch(),
                                 query.queryItemValue(QStringLiteral("npLanguage")),
                                 /*withMessage=*/true));
            return JsonOk(body);
        });

    // GET /v1/users/me/gameCustomData/items/<itemId>/gameData
    http.route(
        "/v1/users/<arg>/gameCustomData/items/<arg>/gameData", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey, const QString& id,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (auto e = CheckCaller(auth, userKey))
                return std::move(*e);
            const auto itemId = ParseItemId(id);
            if (!itemId)
                return Error(Status::BadRequest, GCD_INVALID_URL_PARAM,
                             QStringLiteral("Invalid parameter in URL (parameter: 'itemId')"));
            GameCustomDataDb gdb(db.Conn());
            const auto item =
                gdb.GetReceived(*auth.userId, *itemId, CallerScope(shared, *auth.userId));
            // URL items have no game data, and data not meant for this platform can't be read.
            if (!item || item->IsUrl() || !UsableHere(*item))
                return NotFound(*itemId);
            if (item->IsExpired(QDateTime::currentSecsSinceEpoch()))
                return Error(Status::Forbidden, GCD_EXPIRED,
                             QStringLiteral("Expired game custom data"));
            if (item->used)
                return Error(Status::Forbidden, GCD_ALREADY_USED,
                             QStringLiteral("Already used game custom data"));
            const auto data = gdb.GameData(*itemId);
            if (!data)
                return NotFound(*itemId);
            qInfo() << "WebAPI: gameCustomData item" << *itemId << "game data ->" << auth.npid
                    << data->size() << "bytes";
            return QHttpServerResponse{"application/octet-stream", *data, Status::Ok};
        });

    // PUT /v1/users/me/gameCustomData/items/<itemId>  {"dataUsedFlag":true}
    http.route(
        "/v1/users/<arg>/gameCustomData/items/<arg>", QHttpServerRequest::Method::Put,
        [&db, &shared](const QString& userKey, const QString& id,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (auto e = CheckCaller(auth, userKey))
                return std::move(*e);
            const auto itemId = ParseItemId(id);
            if (!itemId)
                return Error(Status::BadRequest, GCD_INVALID_URL_PARAM,
                             QStringLiteral("Invalid parameter in URL (parameter: 'itemId')"));

            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject())
                return Error(Status::BadRequest, GCD_INVALID_BODY, QStringLiteral("Invalid body"));
            const QJsonValue flag = doc.object().value(QStringLiteral("dataUsedFlag"));
            if (flag.isUndefined())
                return BodyParamRequired(QStringLiteral("dataUsedFlag"));
            if (!flag.isBool() || !flag.toBool())
                return BodyParam(QStringLiteral("dataUsedFlag"));

            GameCustomDataDb gdb(db.Conn());
            const auto item =
                gdb.GetReceived(*auth.userId, *itemId, CallerScope(shared, *auth.userId));
            if (!item)
                return NotFound(*itemId);
            if (item->IsUrl())
                return Error(Status::BadRequest, GCD_URL_FLAG,
                             QStringLiteral("Failed to update flag(type=\"url\")"));
            if (item->IsExpired(QDateTime::currentSecsSinceEpoch()))
                return Error(Status::Forbidden, GCD_EXPIRED,
                             QStringLiteral("Expired game custom data"));
            if (item->used || !gdb.MarkUsed(*auth.userId, *itemId))
                return Error(Status::Forbidden, GCD_ALREADY_USED,
                             QStringLiteral("Already used game custom data"));
            qInfo() << "WebAPI: gameCustomData item" << *itemId << "used by" << auth.npid;
            return QHttpServerResponse{Status::NoContent};
        });

    // GET /v1/users/me/gameCustomData/items/<itemId>/thumbnail -- shadNet extension for the
    // emulator's receive dialog (the system UI shows thumbnails, games never ask for them).
    http.route(
        "/v1/users/<arg>/gameCustomData/items/<arg>/thumbnail", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& userKey, const QString& id,
                       const QHttpServerRequest& req) -> QHttpServerResponse {
            auto auth = WebApiAuth::Authenticate(req, db);
            if (!auth.userId.has_value())
                return std::move(auth.errorResponse);
            if (auto e = CheckCaller(auth, userKey))
                return std::move(*e);
            const auto itemId = ParseItemId(id);
            if (!itemId)
                return Error(Status::BadRequest, GCD_INVALID_URL_PARAM,
                             QStringLiteral("Invalid parameter in URL (parameter: 'itemId')"));
            GameCustomDataDb gdb(db.Conn());
            if (!gdb.GetReceived(*auth.userId, *itemId, CallerScope(shared, *auth.userId)))
                return NotFound(*itemId);
            const auto image = gdb.Thumbnail(*itemId);
            if (!image)
                return NotFound(*itemId);
            const QByteArray type = image->startsWith("\x89PNG") ? "image/png" : "image/jpeg";
            return QHttpServerResponse{type, *image, Status::Ok};
        });
}

} // namespace WebApiRoutes
