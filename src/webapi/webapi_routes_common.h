// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QtGlobal>

#include "avatar.h"

// Helpers shared across the per-resource WebAPI route files (users, profile, ...).
// All inline so the header can be included by multiple translation units.
namespace WebApiRoutes {

// User Profile Web API error codes.
inline constexpr quint32 UP_INVALID_QUERY_PARAM = 2105601;       // bad value in query string
inline constexpr quint32 UP_QUERY_PARAM_REQUIRED = 2105605;      // required query param missing
inline constexpr quint32 UP_ACCESS_DENIED_OWNERSHIP = 2105358;   // non-owner accessed the resource
inline constexpr quint32 UP_NON_FRIEND_NOT_ALLOWED = 2107904;    // non-owner/non-friend access
inline constexpr quint32 UP_USER_NOT_FOUND = 2105356;            // target user does not exist
inline constexpr quint32 UP_INVALID_BODY = 2105728;              // request body value invalid
inline constexpr quint32 UP_BODY_PARAM_REQUIRED = 2105734;       // required body member missing
inline constexpr quint32 UP_INVALID_QUERY_COMBINATION = 2105602; // query params can't be combined
inline constexpr quint32 UP_INVALID_BODY_PARAM = 2105730;        // body member value invalid
inline constexpr quint32 UP_INVALID_URL_PARAM = 2105604;         // value in the URL path invalid
inline constexpr quint32 UP_INTERNAL_ERROR = 2106113;            // internal server error

// Region is not stored per account yet. The User Profile API encodes npId as
// base64("<onlineId>.<region>"), so we default the region here. Change this (or
// promote it to an account column via a migration) when regions are tracked.
inline constexpr char DefaultRegion[] = "us";

inline QHttpServerResponse JsonError(QHttpServerResponse::StatusCode status, quint32 code,
                                     const QString& message) {
    QJsonObject errorObj;
    errorObj.insert(QStringLiteral("code"), static_cast<qint64>(code));
    errorObj.insert(QStringLiteral("message"), message);
    QJsonObject body;
    body.insert(QStringLiteral("error"), errorObj);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

// Parses a request body (or multipart part) as JSON.
inline QJsonDocument ParseJsonBody(const QByteArray& raw, QJsonParseError* error) {
    qsizetype begin = 0;
    qsizetype end = raw.size();
    if (raw.startsWith("\xEF\xBB\xBF")) {
        begin = 3;
    }
    while (end > begin) {
        const char c = raw.at(end - 1);
        if (c != '\0' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        --end;
    }
    return QJsonDocument::fromJson(raw.mid(begin, end - begin), error);
}

inline QHttpServerResponse JsonOk(const QJsonObject& body) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

// Warn about any present query parameter we don't honor, so unsupported params
// are visible in the log rather than silently ignored.
inline void LogUnsupportedQueryParams(const QHttpServerRequest& req, const QSet<QString>& known) {
    const QUrlQuery query(req.url());
    QStringList ignored;
    const auto items = query.queryItems();
    for (const auto& kv : items) {
        if (!known.contains(kv.first)) {
            ignored << kv.first;
        }
    }
    if (!ignored.isEmpty()) {
        qWarning() << "WebAPI:" << req.url().path()
                   << "ignored params:" << ignored.join(QStringLiteral(", "));
    }
}

inline QString EncodeNpId(const QString& onlineId) {
    return QString::fromLatin1((onlineId + QLatin1Char('@')).toUtf8().toBase64());
}

// Parse limit (clamped to [0, maxLimit], default fallbackDefault) and offset (>=0).
inline void ParsePaging(const QUrlQuery& query, int fallbackDefault, int maxLimit, int& limit,
                        int& offset) {
    bool ok = false;
    limit = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
    if (!ok) {
        limit = fallbackDefault;
    }
    limit = qBound(0, limit, maxLimit);
    offset = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
    if (!ok || offset < 0) {
        offset = 0;
    }
}

inline QJsonObject MakePresenceEntry(bool online, const QString& platform,
                                     const QString& gameStatus, const QString& gameData,
                                     const QString& npTitleId, const QString& titleName,
                                     bool includeDetail, bool forcePlatform) {
    QJsonObject e;
    e.insert(QStringLiteral("onlineStatus"),
             online ? QStringLiteral("online") : QStringLiteral("offline"));
    if (online || forcePlatform) {
        e.insert(QStringLiteral("platform"), platform.isEmpty() ? QStringLiteral("PS4") : platform);
    }
    if (online && includeDetail) {
        if (!gameStatus.isEmpty()) {
            e.insert(QStringLiteral("gameStatus"), gameStatus);
        }
        if (!gameData.isEmpty()) {
            e.insert(QStringLiteral("gameData"), gameData);
        }
        if (!npTitleId.isEmpty() || !titleName.isEmpty()) {
            QJsonObject gti;
            if (!npTitleId.isEmpty())
                gti.insert(QStringLiteral("npTitleId"), npTitleId);
            if (!titleName.isEmpty())
                gti.insert(QStringLiteral("titleName"), titleName);
            e.insert(QStringLiteral("gameTitleInfo"), gti);
        }
    }
    return e;
}

// Full presence object: {onlineStatus, [platform], <primaryInfo | platformInfoList |
// incontextInfoList>} selected by presenceType (default primary).
inline QJsonObject MakePresence(const QString& presenceType, bool online, const QString& platform,
                                const QString& gameStatus, const QString& gameData,
                                const QString& npTitleId, const QString& titleName, bool detail,
                                bool inSameGame, const QString& platReq) {
    QJsonObject presence;
    presence.insert(QStringLiteral("onlineStatus"),
                    online ? QStringLiteral("online") : QStringLiteral("offline"));
    const bool isPrimary =
        presenceType != QStringLiteral("platform") && presenceType != QStringLiteral("incontext");
    if (detail && online && !isPrimary) {
        presence.insert(QStringLiteral("platform"),
                        platform.isEmpty() ? QStringLiteral("PS4") : platform);
    }
    const bool platOk = platReq.isEmpty() || platReq == QStringLiteral("PS4");
    if (presenceType == QStringLiteral("platform")) {
        QJsonArray list;
        if (platOk)
            list.append(MakePresenceEntry(online, QStringLiteral("PS4"), gameStatus,
                                          detail ? gameData : QString(), npTitleId, titleName,
                                          /*includeDetail=*/true, /*forcePlatform=*/true));
        presence.insert(QStringLiteral("platformInfoList"), list);
    } else if (presenceType == QStringLiteral("incontext")) {
        QJsonArray list;
        if (online && inSameGame && platOk)
            list.append(MakePresenceEntry(online, QStringLiteral("PS4"), gameStatus, gameData,
                                          npTitleId, titleName, /*includeDetail=*/true,
                                          /*forcePlatform=*/true));
        presence.insert(QStringLiteral("incontextInfoList"), list);
    } else { // primary (default)
        presence.insert(QStringLiteral("primaryInfo"),
                        MakePresenceEntry(online, platform, gameStatus,
                                          detail ? gameData : QString(), npTitleId, titleName,
                                          /*includeDetail=*/true, /*forcePlatform=*/false));
    }
    return presence;
}

// Avatar sizes : s = 50x50, m = 160x160, l = 240x240.
inline bool IsAvatarSize(const QString& size) {
    return Avatar::IsSize(size);
}

struct AvatarRequest {
    bool wanted = false; // fields asked for avatarUrl or avatarUrls
    bool multi = false;  // avatarUrls requested (wins over avatarUrl)
    QStringList sizes;   // one entry unless multi; defaults to "l"
    bool secure = false;
    QString error; // non-empty: reject the request with this message
    quint32 errorCode = 0;
};

inline AvatarRequest ParseAvatarRequest(const QUrlQuery& query, const QStringList& fields,
                                        bool fieldsWantAvatar) {
    AvatarRequest r;
    r.wanted = fieldsWantAvatar;
    r.multi = fields.contains(QStringLiteral("avatarUrls"));
    r.secure = query.queryItemValue(QStringLiteral("avatarUrlScheme")) == QStringLiteral("secure");
    const bool hasSize = query.hasQueryItem(QStringLiteral("avatarSize"));
    const bool hasSizes = query.hasQueryItem(QStringLiteral("avatarSizes"));
    if (hasSize && hasSizes) {
        r.errorCode = UP_INVALID_QUERY_COMBINATION;
        r.error = QStringLiteral("Invalid parameter combination in query string (parameters: "
                                 "'avatarSize', 'avatarSizes')");
        return r;
    }
    if (hasSizes && !r.multi) {
        r.errorCode = UP_INVALID_QUERY_COMBINATION;
        r.error = QStringLiteral("Invalid parameter combination in query string (parameters: "
                                 "'avatarSizes', 'fields')");
        return r;
    }
    const QString raw = query.queryItemValue(hasSizes ? QStringLiteral("avatarSizes")
                                                      : QStringLiteral("avatarSize"));
    for (const QString& size : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (!IsAvatarSize(size) || (hasSize && !r.sizes.isEmpty())) {
            r.errorCode = UP_INVALID_QUERY_PARAM;
            r.error =
                QStringLiteral("Invalid parameter in query string (parameter: '%1')")
                    .arg(hasSizes ? QStringLiteral("avatarSizes") : QStringLiteral("avatarSize"));
            return r;
        }
        if (!r.sizes.contains(size))
            r.sizes.append(size);
    }
    if (r.sizes.isEmpty())
        r.sizes.append(QStringLiteral("l"));
    return r;
}

inline QString AvatarUrlForSize(const QString& storedAvatar, const QString& size, bool secure) {
    QString url = Avatar::Url(storedAvatar, size);
    if (secure && url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive))
        url = QStringLiteral("https://") + url.mid(7);
    return url;
}

inline void InsertAvatar(QJsonObject& entry, const QString& storedAvatar,
                         const AvatarRequest& req) {
    if (!req.wanted || storedAvatar.isEmpty())
        return;
    if (req.multi) {
        QJsonArray urls;
        for (const QString& size : req.sizes) {
            QJsonObject u;
            u.insert(QStringLiteral("avatarUrl"), AvatarUrlForSize(storedAvatar, size, req.secure));
            u.insert(QStringLiteral("size"), size);
            urls.append(u);
        }
        entry.insert(QStringLiteral("avatarUrls"), urls);
    } else {
        entry.insert(QStringLiteral("avatarUrl"),
                     AvatarUrlForSize(storedAvatar, req.sizes.first(), req.secure));
    }
}

} // namespace WebApiRoutes
