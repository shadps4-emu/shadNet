// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QtGlobal>

// Helpers shared across the per-resource WebAPI route files (users, profile, ...).
// All inline so the header can be included by multiple translation units.
namespace WebApiRoutes {

// User Profile Web API error codes.
inline constexpr quint32 UP_INVALID_QUERY_PARAM = 2105601;     // bad value in query string
inline constexpr quint32 UP_QUERY_PARAM_REQUIRED = 2105605;    // required query param missing
inline constexpr quint32 UP_ACCESS_DENIED_OWNERSHIP = 2105358; // non-owner accessed the resource
inline constexpr quint32 UP_NON_FRIEND_NOT_ALLOWED = 2107904;  // non-owner/non-friend access

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

// npId = base64("<onlineId>"). sceNpWebApiUtilityParseNpId takes the part before
// '@' as the handle; with no '@' the whole decoded string is the online-id handle.
inline QString EncodeNpId(const QString& onlineId) {
    return QString::fromLatin1(onlineId.toUtf8().toBase64());
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

// One presence entry: {onlineStatus, [platform], [gameTitleInfo], [gameStatus]}. Used as
// primaryInfo and as a platformInfoList/incontextInfoList element. Detail (gameStatus /
// gameTitleInfo) is included only when includeDetail is set; platform is emitted when the
// user is online, or when forcePlatform is set (per-platform lists name the platform even
// when offline). gameStatus is the only multilingual field (npLanguages applies to it).
inline QJsonObject MakePresenceEntry(bool online, const QString& platform,
                                     const QString& gameStatus, const QString& npTitleId,
                                     const QString& titleName, bool includeDetail,
                                     bool forcePlatform) {
    QJsonObject e;
    e.insert(QStringLiteral("onlineStatus"),
             online ? QStringLiteral("online") : QStringLiteral("offline"));
    if (online || forcePlatform) {
        e.insert(QStringLiteral("platform"),
                 platform.isEmpty() ? QStringLiteral("PS4") : platform);
    }
    if (online && includeDetail) {
        if (!gameStatus.isEmpty()) {
            e.insert(QStringLiteral("gameStatus"), gameStatus);
        }
        if (!npTitleId.isEmpty() || !titleName.isEmpty()) {
            QJsonObject gti;
            if (!npTitleId.isEmpty()) gti.insert(QStringLiteral("npTitleId"), npTitleId);
            if (!titleName.isEmpty()) gti.insert(QStringLiteral("titleName"), titleName);
            e.insert(QStringLiteral("gameTitleInfo"), gti);
        }
    }
    return e;
}

// friendList embeds presence as {"primaryInfo": <entry>} (detail always included).
inline QJsonObject MakePresenceObject(bool online, const QString& platform,
                                      const QString& gameStatus, const QString& npTitleId,
                                      const QString& titleName) {
    QJsonObject presence;
    presence.insert(QStringLiteral("primaryInfo"),
                    MakePresenceEntry(online, platform, gameStatus, npTitleId, titleName,
                                      /*includeDetail=*/true, /*forcePlatform=*/false));
    return presence;
}

} // namespace WebApiRoutes
