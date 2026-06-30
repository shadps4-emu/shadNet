// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_session.h"

#include <QDateTime>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>

#include "client_session.h"
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

constexpr quint32 SESSION_NOT_PERMITTED = 2114560;
constexpr quint32 SESSION_FULL = 2114561;
constexpr quint32 SESSION_ONLY_CREATOR = 2114562;
constexpr quint32 SESSION_ONLY_MEMBER = 2114563;
constexpr quint32 SESSION_USER_NOT_ONLINE = 2114564;
constexpr quint32 SESSION_ALREADY_JOINED = 2114565;

// Common Web API request errors
constexpr quint32 WEBAPI_TOO_LARGE_BODY = 2113539;       // request body too long
constexpr quint32 WEBAPI_RESOURCE_NOT_FOUND = 2113549;   // target resource does not exist
constexpr quint32 WEBAPI_QUERY_PARAM_REQUIRED = 2113797; // required query parameter missing
constexpr quint32 WEBAPI_INVALID_BODY = 2113920;         // request body value invalid
constexpr quint32 WEBAPI_INVALID_BODY_PARAM = 2113922;   // body member value invalid
constexpr quint32 WEBAPI_INVALID_NUM_ELEMS = 2113923;    // array element count invalid
constexpr quint32 WEBAPI_BODY_PARAM_REQUIRED = 2113926;  // required body member missing

// Invitation-specific error
constexpr quint32 INVITATION_EXPIRED = 2115584; // invitation expired or data already used

// 0 == permitted. owner-bind: only the creator may mutate/delete; owner-migration: any member.
quint32 SessionPermissionError(const SharedState::Session& s, int64_t userId) {
    const bool isOwner = (s.ownerUserId == userId);
    bool isMember = false;
    for (const auto& m : s.members) {
        if (m.userId == userId) {
            isMember = true;
            break;
        }
    }
    if (s.sessionType == QStringLiteral("owner-bind"))
        return isOwner ? 0u : SESSION_ONLY_CREATOR;
    return isMember ? 0u : SESSION_ONLY_MEMBER;
}

QHttpServerResponse SessionPermissionResponse(quint32 err) {
    return JsonError(QHttpServerResponse::StatusCode::Forbidden, err,
                     err == SESSION_ONLY_CREATOR
                         ? QStringLiteral("Only the session creator is permitted to perform "
                                          "this operation")
                         : QStringLiteral("Only session members are permitted to perform "
                                          "this operation"));
}

constexpr int kImageMax = 160 * 1024;
constexpr int kDataMax = 1024 * 1024;
constexpr int kChangeableMax = 1024;
constexpr int kInvitationDataMax = 1024 * 1024;

struct MultipartPart {
    QByteArray contentType;
    QByteArray contentDescription;
    QByteArray data;
};

// boundary from "multipart/mixed; boundary=abc" (boundary may be quoted / have trailing params).
QByteArray ExtractBoundary(const QByteArray& contentType) {
    const int b = contentType.indexOf("boundary=");
    if (b < 0)
        return {};
    QByteArray v = contentType.mid(b + 9).trimmed();
    const int semi = v.indexOf(';');
    if (semi >= 0)
        v = v.left(semi).trimmed();
    if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
        v = v.mid(1, v.size() - 2);
    return v;
}

// Minimal multipart/mixed splitter: parts delimited by "--boundary", each with its own headers
// (terminated by a blank line) then a body; the stream ends at "--boundary--".
QList<MultipartPart> ParseMultipartMixed(const QByteArray& body, const QByteArray& boundary) {
    QList<MultipartPart> parts;
    const QByteArray delim = "--" + boundary;
    int pos = body.indexOf(delim);
    if (pos < 0)
        return parts;
    pos += delim.size();
    while (pos < body.size()) {
        if (body.mid(pos, 2) == "--")
            break; // closing delimiter
        if (body.mid(pos, 2) == "\r\n")
            pos += 2;
        else if (pos < body.size() && body[pos] == '\n')
            pos += 1;

        int sep = 4;
        int hdrEnd = body.indexOf("\r\n\r\n", pos);
        if (hdrEnd < 0) {
            hdrEnd = body.indexOf("\n\n", pos);
            sep = 2;
        }
        if (hdrEnd < 0)
            break;
        const QByteArray headers = body.mid(pos, hdrEnd - pos);
        const int bodyStart = hdrEnd + sep;
        const int next = body.indexOf(delim, bodyStart);
        if (next < 0)
            break;
        int bodyEnd = next;
        if (bodyEnd >= 2 && body.mid(bodyEnd - 2, 2) == "\r\n")
            bodyEnd -= 2;
        else if (bodyEnd >= 1 && body[bodyEnd - 1] == '\n')
            bodyEnd -= 1;

        MultipartPart part;
        part.data = body.mid(bodyStart, bodyEnd - bodyStart);
        for (const QByteArray& raw : headers.split('\n')) {
            const QByteArray line = raw.trimmed();
            const int c = line.indexOf(':');
            if (c < 0)
                continue;
            const QByteArray name = line.left(c).trimmed().toLower();
            const QByteArray val = line.mid(c + 1).trimmed();
            if (name == "content-type")
                part.contentType = val;
            else if (name == "content-description")
                part.contentDescription = val;
        }
        parts.append(part);
        pos = next + delim.size();
    }
    return parts;
}

// Remove (userId, index) from any session it currently occupies, applying the deletion rules:
//   owner-bind      -> session is deleted when its creator (owner) leaves;
//   owner-migration -> session is deleted when it reaches 0 members, else ownership migrates.
// Caller must hold sessionsLock for writing.
void LeaveSessionAtIndex(SharedState& shared, int64_t userId, int index) {
    QString leaveId;
    for (auto it = shared.sessions.cbegin(); it != shared.sessions.cend() && leaveId.isEmpty();
         ++it) {
        for (const auto& m : it.value().members) {
            if (m.userId == userId && m.index == index) {
                leaveId = it.key();
                break;
            }
        }
    }
    if (leaveId.isEmpty())
        return;
    auto& s = shared.sessions[leaveId];
    const bool wasOwner = (s.ownerUserId == userId);
    for (int m = 0; m < s.members.size(); ++m) {
        if (s.members[m].userId == userId && s.members[m].index == index) {
            s.members.removeAt(m);
            break;
        }
    }
    if ((s.sessionType == QStringLiteral("owner-bind") && wasOwner) || s.members.isEmpty()) {
        shared.sessions.remove(leaveId);
    } else if (wasOwner) {
        // owner-migration: hand ownership to the next remaining member.
        s.ownerUserId = s.members.first().userId;
        s.ownerNpid = s.members.first().npid;
    }
}

// Resolve a <arg> path segment (accountId | onlineId | "me") to a numeric account id.
// Returns false when the handle is unknown.
bool ResolveTarget(Database& db, const QString& userKey, const WebApiAuth::AuthResult& auth,
                   int64_t& targetId) {
    if (userKey.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
        userKey.compare(auth.npid, Qt::CaseInsensitive) == 0) {
        targetId = *auth.userId;
        return true;
    }
    bool num = false;
    const qlonglong v = userKey.toLongLong(&num);
    if (num) {
        targetId = v;
        return true;
    }
    const auto u = db.GetUserId(userKey);
    if (!u)
        return false;
    targetId = *u;
    return true;
}

// GET /v1/users/<arg>/sessions -- the target user's sessions (as sessionEntity rows). Private
// sessions are visible only to participants (invitations are not modeled), so inaccessible
// private sessions are filtered out rather than returned
// One of a user's visible sessions, with every field the session-list endpoints can emit.
struct SessionRow {
    QString sessionId;
    QString platform;
    QString sessionName;
    QHash<QString, QString> locName;
    qint64 createdAt = 0;
    QStringList availablePlatforms;
    int memberCount = 0;
    bool lockFlag = false;
    int index = 0;
    int priority = 0;
    qint64 joinedAt = 0;
};

// Collect targetId's sessions visible to callerId (public, or private when the caller is a
// participant), then select either the indices in indexFilter or, with
// no filter, the single highest-priority + most-recently-joined session. Shared by the single- and
// multi-user session-list endpoints so privacy/selection stay identical.
QList<SessionRow> CollectUserSessions(SharedState& shared, int64_t targetId, int64_t callerId,
                                      bool hasIndexFilter, const QSet<int>& indexFilter) {
    QList<SessionRow> rows;
    {
        QReadLocker lk(&shared.sessionsLock);
        for (auto it = shared.sessions.cbegin(); it != shared.sessions.cend(); ++it) {
            const auto& s = it.value();
            const SharedState::SessionMember* tm = nullptr;
            bool callerIsMember = false;
            for (const auto& m : s.members) {
                if (m.userId == targetId)
                    tm = &m;
                if (m.userId == callerId)
                    callerIsMember = true;
            }
            if (!tm)
                continue;
            if (s.sessionPrivacy == QStringLiteral("private") && !callerIsMember)
                continue;
            SessionRow r;
            r.sessionId = s.sessionId;
            r.platform = tm->platform;
            r.sessionName = s.sessionName;
            r.locName = s.localizedSessionNames;
            r.createdAt = s.createdAt;
            r.availablePlatforms = s.availablePlatforms;
            r.memberCount = static_cast<int>(s.members.size());
            r.lockFlag = s.sessionLockFlag;
            r.index = tm->index;
            r.priority = tm->priority;
            r.joinedAt = tm->joinedAt;
            rows.append(r);
        }
    }
    QList<SessionRow> selected;
    if (hasIndexFilter) {
        for (const auto& r : rows)
            if (indexFilter.contains(r.index))
                selected.append(r);
    } else {
        const SessionRow* best = nullptr;
        for (const auto& r : rows) {
            if (!best || r.priority > best->priority ||
                (r.priority == best->priority && r.joinedAt > best->joinedAt))
                best = &r;
        }
        if (best)
            selected.append(*best);
    }
    return selected;
}

// Parse an "index" query item (comma-separated indices) into a filter set.
QSet<int> ParseIndexFilter(const QUrlQuery& query, bool& hasFilter) {
    hasFilter = query.hasQueryItem(QStringLiteral("index"));
    QSet<int> f;
    if (hasFilter) {
        for (const QString& tok : query.queryItemValue(QStringLiteral("index"))
                                      .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            bool ok = false;
            const int i = tok.toInt(&ok);
            if (ok)
                f.insert(i);
        }
    }
    return f;
}

// GET /v1/users/<arg>/sessions -- a single user's sessions (fields: platform,sessionId default;
// index,priority additive).
QHttpServerResponse HandleSessionList(Database& db, SharedState& shared, const QString& userKey,
                                      const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    int64_t targetId = 0;
    const bool resolved = ResolveTarget(db, userKey, auth, targetId);

    const QUrlQuery query(req.url());
    const QStringList fields =
        query.queryItemValue(QStringLiteral("fields")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool isDefault = fields.isEmpty();
    const bool wantPlatform = isDefault || fields.contains(QStringLiteral("platform"));
    const bool wantSessionId = isDefault || fields.contains(QStringLiteral("sessionId"));
    const bool wantIndex = fields.contains(QStringLiteral("index"));
    const bool wantPriority = fields.contains(QStringLiteral("priority"));

    bool hasIndexFilter = false;
    const QSet<int> indexFilter = ParseIndexFilter(query, hasIndexFilter);

    QList<SessionRow> selected;
    if (resolved)
        selected = CollectUserSessions(shared, targetId, *auth.userId, hasIndexFilter, indexFilter);

    QJsonArray arr;
    for (const auto& r : selected) {
        QJsonObject o;
        if (wantPlatform)
            o.insert(QStringLiteral("platform"), r.platform);
        if (wantSessionId)
            o.insert(QStringLiteral("sessionId"), r.sessionId);
        if (wantIndex)
            o.insert(QStringLiteral("index"), r.index);
        if (wantPriority)
            o.insert(QStringLiteral("priority"), r.priority);
        arr.append(o);
    }
    QJsonObject body;
    body.insert(QStringLiteral("sessions"), arr);
    body.insert(QStringLiteral("start"), 0);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    body.insert(QStringLiteral("totalResults"), static_cast<int>(arr.size()));
    qInfo() << "WebAPI: session list for target" << targetId << "->" << arr.size();
    return JsonOk(body);
}

// GET /v1/users/@users/sessions?accountIds=... -- multiple users' sessions. @default ==
// platform,sessionId; sessionName/sessionCreateTimestamp/availablePlatforms/memberCount/
// sessionLockFlag/index/priority are additive. Users with no visible session are omitted; the
// envelope is just {users:[...]} (no start/size/totalResults).
QHttpServerResponse HandleMultiUserSessions(Database& db, SharedState& shared,
                                            const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const QUrlQuery query(req.url());
    if (!query.hasQueryItem(QStringLiteral("accountIds"))) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_QUERY_PARAM_REQUIRED,
                         QStringLiteral("'accountIds' parameter required in query string"));
    }
    const QStringList ids = query.queryItemValue(QStringLiteral("accountIds"))
                                .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (ids.isEmpty() || ids.size() > 50) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_NUM_ELEMS,
                         QStringLiteral("'accountIds' must list 1-50 account IDs"));
    }

    const QStringList fields =
        query.queryItemValue(QStringLiteral("fields")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool isDefault = fields.isEmpty() || fields.contains(QStringLiteral("@default"));
    const bool wantPlatform = isDefault || fields.contains(QStringLiteral("platform"));
    const bool wantSessionId = isDefault || fields.contains(QStringLiteral("sessionId"));
    const bool wantName = fields.contains(QStringLiteral("sessionName"));
    const bool wantCreateTs = fields.contains(QStringLiteral("sessionCreateTimestamp"));
    const bool wantPlatforms = fields.contains(QStringLiteral("availablePlatforms"));
    const bool wantMemberCount = fields.contains(QStringLiteral("memberCount"));
    const bool wantLockFlag = fields.contains(QStringLiteral("sessionLockFlag"));
    const bool wantIndex = fields.contains(QStringLiteral("index"));
    const bool wantPriority = fields.contains(QStringLiteral("priority"));
    const QString npLanguage = query.queryItemValue(QStringLiteral("npLanguage"));

    bool hasIndexFilter = false;
    const QSet<int> indexFilter = ParseIndexFilter(query, hasIndexFilter);

    QJsonArray users;
    for (const QString& idStr : ids) {
        bool ok = false;
        const qlonglong accId = idStr.toLongLong(&ok);
        if (!ok)
            continue;
        const QList<SessionRow> rows =
            CollectUserSessions(shared, accId, *auth.userId, hasIndexFilter, indexFilter);
        if (rows.isEmpty())
            continue; // user with no visible session is omitted
        QJsonArray arr;
        for (const auto& r : rows) {
            QJsonObject o;
            if (wantPlatform)
                o.insert(QStringLiteral("platform"), r.platform);
            if (wantSessionId)
                o.insert(QStringLiteral("sessionId"), r.sessionId);
            if (wantName) {
                QString name = r.sessionName;
                if (!npLanguage.isEmpty()) {
                    const auto n = r.locName.constFind(npLanguage);
                    if (n != r.locName.constEnd())
                        name = n.value();
                }
                o.insert(QStringLiteral("sessionName"), name);
            }
            if (wantCreateTs)
                o.insert(QStringLiteral("sessionCreateTimestamp"), r.createdAt);
            if (wantPlatforms) {
                QJsonArray a;
                for (const auto& pf : r.availablePlatforms)
                    a.append(pf);
                o.insert(QStringLiteral("availablePlatforms"), a);
            }
            if (wantMemberCount)
                o.insert(QStringLiteral("memberCount"), r.memberCount);
            if (wantLockFlag)
                o.insert(QStringLiteral("sessionLockFlag"), r.lockFlag);
            if (wantIndex)
                o.insert(QStringLiteral("index"), r.index);
            if (wantPriority)
                o.insert(QStringLiteral("priority"), r.priority);
            arr.append(o);
        }
        QJsonObject u;
        u.insert(QStringLiteral("onlineId"), db.GetUsername(accId).value_or(QString()));
        u.insert(QStringLiteral("accountId"), QString::number(accId));
        u.insert(QStringLiteral("sessions"), arr);
        users.append(u);
    }
    QJsonObject body;
    body.insert(QStringLiteral("users"), users);
    qInfo() << "WebAPI: multi-user session list ->" << users.size() << "user(s)";
    return JsonOk(body);
}

QHttpServerResponse HandleSessionCreate(Database& db, SharedState& shared,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }

    const QByteArray boundary = ExtractBoundary(req.value("Content-Type"));
    if (boundary.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Expected multipart/mixed body with a boundary"));
    }
    const QList<MultipartPart> parts = ParseMultipartMixed(req.body(), boundary);

    QByteArray jsonPart, imagePart, dataPart, changeablePart;
    bool haveJson = false, haveImage = false, haveData = false, haveChangeable = false;
    for (const auto& p : parts) {
        const QByteArray& d = p.contentDescription;
        if (d == "session-request") {
            jsonPart = p.data;
            haveJson = true;
        } else if (d == "session-image") {
            imagePart = p.data;
            haveImage = true;
        } else if (d == "session-data") {
            dataPart = p.data;
            haveData = true;
        } else if (d == "changeable-session-data") {
            changeablePart = p.data;
            haveChangeable = true;
        }
    }
    // session-request and session-image are required; at least one data part is required.
    if (!haveJson || !haveImage || (!haveData && !haveChangeable)) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_BODY_PARAM_REQUIRED,
                         QStringLiteral("Missing required multipart part"));
    }
    if (imagePart.size() > kImageMax || dataPart.size() > kDataMax ||
        changeablePart.size() > kChangeableMax) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_TOO_LARGE_BODY,
                         QStringLiteral("Multipart part exceeds the size limit"));
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPart, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Malformed session-request JSON"));
    }
    const QJsonObject obj = doc.object();

    const QString sessionType = obj.value(QStringLiteral("sessionType")).toString();
    const QString sessionPrivacy = obj.value(QStringLiteral("sessionPrivacy")).toString();
    const int sessionMaxUser = obj.value(QStringLiteral("sessionMaxUser")).toInt();
    QStringList platforms;
    for (const auto& v : obj.value(QStringLiteral("availablePlatforms")).toArray())
        platforms.append(v.toString());

    // Required-field validation per the spec.
    if ((sessionType != QStringLiteral("owner-bind") &&
         sessionType != QStringLiteral("owner-migration")) ||
        sessionMaxUser <= 0 || platforms.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY_PARAM,
                         QStringLiteral("Invalid or missing session-request field"));
    }

    // The current user must be online; capture their platform for the membership entry.
    QString userPlatform;
    {
        QReadLocker lk(&shared.clientsLock);
        auto it = shared.clients.constFind(*auth.userId);
        if (it == shared.clients.constEnd()) {
            return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_USER_NOT_ONLINE,
                             QStringLiteral("The current user is not online"));
        }
        userPlatform = it->platform;
    }
    // The creator's platform must be among availablePlatforms.
    if (!platforms.contains(userPlatform)) {
        return JsonError(
            QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY_PARAM,
            QStringLiteral("availablePlatforms must include the current user's platform"));
    }

    const int index = obj.value(QStringLiteral("index")).toInt(0);
    const int priority = obj.value(QStringLiteral("priority")).toInt(0);
    const QString sessionId =
        QStringLiteral("001-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    SharedState::Session s;
    s.sessionId = sessionId;
    s.ownerUserId = *auth.userId;
    s.ownerNpid = auth.npid;
    s.sessionName = obj.value(QStringLiteral("sessionName")).toString();
    s.sessionStatus = obj.value(QStringLiteral("sessionStatus")).toString();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        // localizedSessionNames / localizedSessionStatus are {npLanguage: text} objects.
        if (it.key() == QStringLiteral("localizedSessionNames") && it.value().isObject()) {
            const QJsonObject lo = it.value().toObject();
            for (auto l = lo.constBegin(); l != lo.constEnd(); ++l)
                s.localizedSessionNames.insert(l.key(), l.value().toString());
        } else if (it.key() == QStringLiteral("localizedSessionStatus") && it.value().isObject()) {
            const QJsonObject lo = it.value().toObject();
            for (auto l = lo.constBegin(); l != lo.constEnd(); ++l)
                s.localizedSessionStatus.insert(l.key(), l.value().toString());
        }
    }
    s.sessionType = sessionType;
    s.sessionPrivacy = sessionPrivacy.isEmpty() ? QStringLiteral("public") : sessionPrivacy;
    s.sessionMaxUser = sessionMaxUser;
    s.availablePlatforms = platforms;
    s.npTitleId = obj.value(QStringLiteral("npTitleId")).toString();
    s.sessionLockFlag = obj.value(QStringLiteral("sessionLockFlag")).toBool(false);
    s.sendNotificationFlag = obj.value(QStringLiteral("sendNotificationFlag")).toBool(false);
    s.sessionImage = imagePart;
    s.sessionData = dataPart;
    s.changeableSessionData = changeablePart;
    s.createdAt = now;
    s.members.append({*auth.userId, auth.npid, userPlatform, index, priority, now});

    {
        QWriteLocker lk(&shared.sessionsLock);
        // Re-using an index the user already occupies leaves that session first.
        LeaveSessionAtIndex(shared, *auth.userId, index);
        shared.sessions.insert(sessionId, s);
    }

    qInfo() << "WebAPI: session created" << sessionId << "by" << auth.npid << "type" << sessionType
            << "index" << index;
    QJsonObject body;
    body.insert(QStringLiteral("sessionId"), sessionId);
    return JsonOk(body);
}

// PUT /v1/sessions/<arg> -- update session info (application/json body). owner-bind sessions
// may be updated only by the creator; owner-migration sessions by any participant. The
// sessionName/localizedSessionNames pair (and sessionStatus/localizedSessionStatus) is
// replaced as a unit: supplying one without the other clears the omitted half.
QHttpServerResponse HandleSessionUpdate(Database& db, SharedState& shared, const QString& sessionId,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Malformed session-request JSON"));
    }
    const QJsonObject obj = doc.object();

    QWriteLocker lk(&shared.sessionsLock);
    auto it = shared.sessions.find(sessionId);
    if (it == shared.sessions.end()) {
        return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                         QStringLiteral("Session not found"));
    }
    auto& s = it.value();

    if (const quint32 err = SessionPermissionError(s, *auth.userId))
        return SessionPermissionResponse(err);

    // sessionName / localizedSessionNames: replaced together when either is present.
    if (obj.contains(QStringLiteral("sessionName")) ||
        obj.contains(QStringLiteral("localizedSessionNames"))) {
        s.sessionName = obj.value(QStringLiteral("sessionName")).toString();
        s.localizedSessionNames.clear();
        const QJsonObject lo = obj.value(QStringLiteral("localizedSessionNames")).toObject();
        for (auto l = lo.constBegin(); l != lo.constEnd(); ++l)
            s.localizedSessionNames.insert(l.key(), l.value().toString());
    }
    // sessionStatus / localizedSessionStatus: same paired-replacement rule.
    if (obj.contains(QStringLiteral("sessionStatus")) ||
        obj.contains(QStringLiteral("localizedSessionStatus"))) {
        s.sessionStatus = obj.value(QStringLiteral("sessionStatus")).toString();
        s.localizedSessionStatus.clear();
        const QJsonObject lo = obj.value(QStringLiteral("localizedSessionStatus")).toObject();
        for (auto l = lo.constBegin(); l != lo.constEnd(); ++l)
            s.localizedSessionStatus.insert(l.key(), l.value().toString());
    }
    // Independent fields: updated only when present. sessionType and sendNotificationFlag are
    // fixed at creation and not updatable here.
    if (obj.contains(QStringLiteral("sessionPrivacy")))
        s.sessionPrivacy = obj.value(QStringLiteral("sessionPrivacy")).toString();
    if (obj.contains(QStringLiteral("sessionMaxUser")))
        s.sessionMaxUser = obj.value(QStringLiteral("sessionMaxUser")).toInt();
    if (obj.contains(QStringLiteral("sessionLockFlag")))
        s.sessionLockFlag = obj.value(QStringLiteral("sessionLockFlag")).toBool();
    if (obj.contains(QStringLiteral("availablePlatforms"))) {
        QStringList p;
        for (const auto& v : obj.value(QStringLiteral("availablePlatforms")).toArray())
            p.append(v.toString());
        if (!p.isEmpty())
            s.availablePlatforms = p;
    }

    qInfo() << "WebAPI: session updated" << sessionId << "by" << auth.npid;
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

// GET /v1/sessions/<arg> -- session information. Private sessions are accessible only to a
// participant (invitations not modeled) -> otherwise 2114560. @default returns the core
// fields (+ sessionType, per the doc example); members/sessionLockFlag/sendNotificationFlag
// are additive. npLanguage selects a localized sessionName/sessionStatus within @default.
QHttpServerResponse HandleSessionGet(Database& db, SharedState& shared, const QString& sessionId,
                                     const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const QUrlQuery query(req.url());
    const QStringList fields =
        query.queryItemValue(QStringLiteral("fields")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool wantDefault = fields.isEmpty() || fields.contains(QStringLiteral("@default"));
    const bool wantMembers = fields.contains(QStringLiteral("members"));
    const bool wantLockFlag = fields.contains(QStringLiteral("sessionLockFlag"));
    const bool wantNotifyFlag = fields.contains(QStringLiteral("sendNotificationFlag"));
    const QString npLanguage = query.queryItemValue(QStringLiteral("npLanguage"));

    // Snapshot under the read lock (excluding the large blobs), then do DB lookups + build
    // JSON outside the lock.
    struct Snap {
        QString privacy, type, name, status, ownerNpid;
        QHash<QString, QString> locName, locStatus;
        int maxUser = 0;
        qint64 createdAt = 0;
        int64_t ownerUserId = 0;
        QStringList platforms;
        bool lockFlag = false, notifyFlag = false;
        QList<SharedState::SessionMember> members;
    } snap;
    bool found = false, permitted = false;
    {
        QReadLocker lk(&shared.sessionsLock);
        auto it = shared.sessions.constFind(sessionId);
        if (it != shared.sessions.constEnd()) {
            found = true;
            const auto& s = it.value();
            bool callerIsMember = false;
            for (const auto& m : s.members)
                if (m.userId == *auth.userId) {
                    callerIsMember = true;
                    break;
                }
            permitted = (s.sessionPrivacy != QStringLiteral("private")) || callerIsMember;
            snap.privacy = s.sessionPrivacy;
            snap.type = s.sessionType;
            snap.name = s.sessionName;
            snap.status = s.sessionStatus;
            snap.ownerNpid = s.ownerNpid;
            snap.locName = s.localizedSessionNames;
            snap.locStatus = s.localizedSessionStatus;
            snap.maxUser = s.sessionMaxUser;
            snap.createdAt = s.createdAt;
            snap.ownerUserId = s.ownerUserId;
            snap.platforms = s.availablePlatforms;
            snap.lockFlag = s.sessionLockFlag;
            snap.notifyFlag = s.sendNotificationFlag;
            snap.members = s.members;
        }
    }
    if (!found) {
        return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                         QStringLiteral("Session not found"));
    }
    if (!permitted) {
        return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_NOT_PERMITTED,
                         QStringLiteral("Not permitted to access the session"));
    }

    QJsonObject body;
    if (wantDefault) {
        QString name = snap.name, status = snap.status;
        if (!npLanguage.isEmpty()) {
            const auto n = snap.locName.constFind(npLanguage);
            if (n != snap.locName.constEnd())
                name = n.value();
            const auto st = snap.locStatus.constFind(npLanguage);
            if (st != snap.locStatus.constEnd())
                status = st.value();
        }
        body.insert(QStringLiteral("sessionPrivacy"), snap.privacy);
        body.insert(QStringLiteral("sessionMaxUser"), snap.maxUser);
        body.insert(QStringLiteral("sessionType"), snap.type);
        body.insert(QStringLiteral("sessionName"), name);
        body.insert(QStringLiteral("sessionStatus"), status);
        body.insert(QStringLiteral("sessionCreateTimestamp"), snap.createdAt);
        QJsonObject creator;
        creator.insert(QStringLiteral("onlineId"),
                       db.GetUsername(snap.ownerUserId).value_or(snap.ownerNpid));
        creator.insert(QStringLiteral("accountId"), QString::number(snap.ownerUserId));
        body.insert(QStringLiteral("sessionCreator"), creator);
        QJsonArray plats;
        for (const auto& p : snap.platforms)
            plats.append(p);
        body.insert(QStringLiteral("availablePlatforms"), plats);
    }
    if (wantMembers) {
        // member object (per the 'member' spec): {accountId (numeric string), onlineId,
        // platform}.
        QJsonArray ms;
        for (const auto& m : snap.members) {
            QJsonObject mo;
            mo.insert(QStringLiteral("accountId"), QString::number(m.userId));
            mo.insert(QStringLiteral("onlineId"), db.GetUsername(m.userId).value_or(m.npid));
            mo.insert(QStringLiteral("platform"), m.platform);
            ms.append(mo);
        }
        body.insert(QStringLiteral("members"), ms);
    }
    if (wantLockFlag)
        body.insert(QStringLiteral("sessionLockFlag"), snap.lockFlag);
    if (wantNotifyFlag)
        body.insert(QStringLiteral("sendNotificationFlag"), snap.notifyFlag);
    return JsonOk(body);
}

// DELETE /v1/sessions/<arg> -- delete a session (same permission rule as update).
QHttpServerResponse HandleSessionDelete(Database& db, SharedState& shared, const QString& sessionId,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    QWriteLocker lk(&shared.sessionsLock);
    auto it = shared.sessions.find(sessionId);
    if (it == shared.sessions.end()) {
        return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                         QStringLiteral("Session not found"));
    }
    if (const quint32 err = SessionPermissionError(it.value(), *auth.userId))
        return SessionPermissionResponse(err);
    shared.sessions.remove(sessionId);
    qInfo() << "WebAPI: session deleted" << sessionId << "by" << auth.npid;
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

// GET /v1/users/<arg>/friends/sessions -- sessions the caller's friends are in, each friend's
// highest-priority session, aggregated by session with the friends grouped. @default ==
// sessionId; other fields additive. sessionApplicationType=party returns empty (shadNet models
// only game sessions). Paginated via limit (<=100) / offset.
QHttpServerResponse HandleFriendsSessions(Database& db, SharedState& shared,
                                          const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const QUrlQuery query(req.url());
    const QStringList fields =
        query.queryItemValue(QStringLiteral("fields")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool isDefault = fields.isEmpty() || fields.contains(QStringLiteral("@default"));
    const bool wantSessionId = isDefault || fields.contains(QStringLiteral("sessionId"));
    const bool wantName = fields.contains(QStringLiteral("sessionName"));
    const bool wantCreateTs = fields.contains(QStringLiteral("sessionCreateTimestamp"));
    const bool wantPlatforms = fields.contains(QStringLiteral("availablePlatforms"));
    const bool wantMemberCount = fields.contains(QStringLiteral("memberCount"));
    const bool wantLockFlag = fields.contains(QStringLiteral("sessionLockFlag"));
    const bool wantFriends = fields.contains(QStringLiteral("friends"));
    const QString npLanguage = query.queryItemValue(QStringLiteral("npLanguage"));
    const bool partyRequested =
        query.queryItemValue(QStringLiteral("sessionApplicationType")) == QStringLiteral("party");

    int limit = 100, offset = 0;
    if (query.hasQueryItem(QStringLiteral("limit"))) {
        bool ok = false;
        const int v = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
        if (ok)
            limit = v < 0 ? 0 : (v > 100 ? 100 : v);
    }
    if (query.hasQueryItem(QStringLiteral("offset"))) {
        bool ok = false;
        const int v = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
        if (ok && v >= 0)
            offset = v;
    }

    struct FriendRef {
        int64_t accountId = 0;
        QString onlineId;
        QString platform;
    };
    // Phase 1: each friend's highest-priority visible session -> the set of result sessions, in
    // friend-list order. details[] holds session info from the first contributor.
    QList<QString> order;
    QHash<QString, SessionRow> details;
    QHash<int64_t, QString> friendNpid; // accountId -> onlineId, for the membership scan below
    if (!partyRequested) {
        const auto rels = db.GetRelationships(*auth.userId);
        for (const auto& f : rels.friends) {
            friendNpid.insert(f.first, f.second);
            const QList<SessionRow> top =
                CollectUserSessions(shared, f.first, *auth.userId, false, {});
            if (top.isEmpty())
                continue;
            const SessionRow& r = top.first();
            if (!details.contains(r.sessionId)) {
                details.insert(r.sessionId, r);
                order.append(r.sessionId);
            }
        }
    }
    // Phase 2: for each result session, the caller's friends who are members (platform = the
    // friend's platform in that session).
    QHash<QString, QList<FriendRef>> sessionFriends;
    if (!order.isEmpty()) {
        QReadLocker lk(&shared.sessionsLock);
        for (const QString& sid : order) {
            const auto it = shared.sessions.constFind(sid);
            if (it == shared.sessions.constEnd())
                continue;
            QList<FriendRef> fl;
            for (const auto& m : it.value().members) {
                const auto fn = friendNpid.constFind(m.userId);
                if (fn != friendNpid.constEnd())
                    fl.append({m.userId, fn.value(), m.platform});
            }
            sessionFriends.insert(sid, fl);
        }
    }

    const int total = static_cast<int>(order.size());
    QJsonArray arr;
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        const SessionRow& r = details[order[i]];
        QJsonObject o;
        if (wantSessionId)
            o.insert(QStringLiteral("sessionId"), r.sessionId);
        if (wantName) {
            QString name = r.sessionName;
            if (!npLanguage.isEmpty()) {
                const auto n = r.locName.constFind(npLanguage);
                if (n != r.locName.constEnd())
                    name = n.value();
            }
            o.insert(QStringLiteral("sessionName"), name);
        }
        if (wantCreateTs)
            o.insert(QStringLiteral("sessionCreateTimestamp"), r.createdAt);
        if (wantPlatforms) {
            QJsonArray pa;
            for (const auto& p : r.availablePlatforms)
                pa.append(p);
            o.insert(QStringLiteral("availablePlatforms"), pa);
        }
        if (wantMemberCount)
            o.insert(QStringLiteral("memberCount"), r.memberCount);
        if (wantLockFlag)
            o.insert(QStringLiteral("sessionLockFlag"), r.lockFlag);
        if (wantFriends) {
            QJsonArray fa;
            for (const auto& fr : sessionFriends.value(order[i])) {
                QJsonObject fo;
                fo.insert(QStringLiteral("onlineId"), fr.onlineId);
                fo.insert(QStringLiteral("accountId"), QString::number(fr.accountId));
                fo.insert(QStringLiteral("platform"), fr.platform);
                fa.append(fo);
            }
            o.insert(QStringLiteral("friends"), fa);
        }
        arr.append(o);
    }
    QJsonObject body;
    body.insert(QStringLiteral("sessions"), arr);
    body.insert(QStringLiteral("start"), offset);
    body.insert(QStringLiteral("size"), static_cast<int>(arr.size()));
    body.insert(QStringLiteral("totalResults"), total);
    qInfo() << "WebAPI: friends sessions ->" << arr.size();
    return JsonOk(body);
}

// POST /v1/sessions/<arg>/members -- join a session. index=[0-63] (default 0), priority (default
// 49). A user holds at most one session per index, so joining at an index already
// used elsewhere leaves that other session first; re-posting to the same session at the same
// index just updates priority (204); a different index for an already-joined session is rejected
// (2114565). Full -> 2114561; caller offline -> 2114564. npTitleId query param (server-side only)
// is accepted and ignored. Body: none. Success: 204.
QHttpServerResponse HandleSessionJoin(Database& db, SharedState& shared, const QString& sessionId,
                                      const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const QUrlQuery query(req.url());
    int index = 0;
    if (query.hasQueryItem(QStringLiteral("index"))) {
        bool ok = false;
        const int v = query.queryItemValue(QStringLiteral("index")).toInt(&ok);
        if (ok)
            index = v;
    }
    int priority = 49;
    if (query.hasQueryItem(QStringLiteral("priority"))) {
        bool ok = false;
        const int v = query.queryItemValue(QStringLiteral("priority")).toInt(&ok);
        if (ok)
            priority = v;
    }
    if (index < 0 || index > 63) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY_PARAM,
                         QStringLiteral("'index' must be in the range 0-63"));
    }

    // The caller must be online; capture their platform for the membership entry.
    QString platform;
    {
        QReadLocker lk(&shared.clientsLock);
        auto it = shared.clients.constFind(*auth.userId);
        if (it == shared.clients.constEnd()) {
            return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_USER_NOT_ONLINE,
                             QStringLiteral("The current user is not online"));
        }
        platform = it->platform;
    }

    QWriteLocker lk(&shared.sessionsLock);
    auto sit = shared.sessions.find(sessionId);
    if (sit == shared.sessions.end()) {
        return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                         QStringLiteral("Session not found"));
    }
    {
        // Already a member? same index -> update priority (204); different index -> 2114565.
        auto& s = sit.value();
        for (auto& m : s.members) {
            if (m.userId == *auth.userId) {
                if (m.index != index) {
                    return JsonError(QHttpServerResponse::StatusCode::Forbidden,
                                     SESSION_ALREADY_JOINED,
                                     QStringLiteral("Session has already been joined in another "
                                                    "index"));
                }
                m.priority = priority;
                m.joinedAt = QDateTime::currentMSecsSinceEpoch();
                qInfo() << "WebAPI: session re-join (priority update)" << sessionId << "by"
                        << auth.npid << "index" << index;
                return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
            }
        }
        // A locked session is closed to new members (max reached, join window ended, etc.);
        // an already-joined member updating priority above is unaffected.
        if (s.sessionLockFlag) {
            return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_NOT_PERMITTED,
                             QStringLiteral("The session is locked"));
        }
        // Not a member -> capacity check (min(sessionMaxUser, 256)).
        const int maxUsers =
            (s.sessionMaxUser > 0 && s.sessionMaxUser < 256) ? s.sessionMaxUser : 256;
        if (s.members.size() >= maxUsers) {
            return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_FULL,
                             QStringLiteral("The session is already full"));
        }
    }
    // Joining at this index frees the index elsewhere first (may delete/migrate that other
    // session). THIS session is untouched since the caller isn't a member of it; re-fetch after.
    LeaveSessionAtIndex(shared, *auth.userId, index);
    {
        auto& s = shared.sessions[sessionId];
        SharedState::SessionMember m;
        m.userId = *auth.userId;
        m.npid = auth.npid;
        m.platform = platform;
        m.index = index;
        m.priority = priority;
        m.joinedAt = QDateTime::currentMSecsSinceEpoch();
        s.members.append(m);
    }
    qInfo() << "WebAPI: session join" << sessionId << "by" << auth.npid << "index" << index
            << "priority" << priority;
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

// GET /v1/sessions/<arg>/sessionData -- the session's binary data blob (set at create), returned
// as application/octet-stream (up to 1 MiB). Access: the caller's platform must be among the
// session's availablePlatforms, and a private session is readable only by a participant
// (invitations not modeled); otherwise 2114560. Changeable session data is a separate endpoint.
QHttpServerResponse HandleSessionGetData(Database& db, SharedState& shared,
                                         const QString& sessionId, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    // Caller's platform (for the availablePlatforms check); empty if offline.
    QString callerPlatform;
    {
        QReadLocker lk(&shared.clientsLock);
        const auto it = shared.clients.constFind(*auth.userId);
        if (it != shared.clients.constEnd())
            callerPlatform = it->platform;
    }

    QByteArray data;
    QStringList platforms;
    QString privacy;
    bool found = false;
    bool callerIsMember = false;
    {
        QReadLocker lk(&shared.sessionsLock);
        const auto it = shared.sessions.constFind(sessionId);
        if (it != shared.sessions.constEnd()) {
            found = true;
            const auto& sn = it.value();
            data = sn.sessionData;
            platforms = sn.availablePlatforms;
            privacy = sn.sessionPrivacy;
            for (const auto& m : sn.members) {
                if (m.userId == *auth.userId) {
                    callerIsMember = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                         QStringLiteral("Session not found"));
    }
    // Platform not allowed on this session -> not permitted.
    if (!callerPlatform.isEmpty() && !platforms.isEmpty() && !platforms.contains(callerPlatform)) {
        return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_NOT_PERMITTED,
                         QStringLiteral("Not permitted to access the session"));
    }
    // Private session is readable only by participants (invitations not modeled).
    if (privacy == QStringLiteral("private") && !callerIsMember) {
        return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_NOT_PERMITTED,
                         QStringLiteral("Not permitted to access the session"));
    }
    qInfo() << "WebAPI: session data for" << sessionId << "->" << data.size() << "bytes";
    return QHttpServerResponse(QByteArrayLiteral("application/octet-stream"), data,
                               QHttpServerResponse::StatusCode::Ok);
}

// DELETE /v1/sessions/<arg>/members/<arg> -- leave a session. The member segment is "me" (or the
// caller's account/onlineId); a member always removes themselves, and the session owner may remove
// another member, otherwise 2114560. Removing the owner migrates ownership (owner-migration) or
// deletes the session (owner-bind, or last member). Idempotent: leaving a session/membership that
// no longer exists still returns 204. Body: none. Success: 204.
// Remove userId from sn if present (sets *outRemoved). Applies owner teardown and returns true
// when the session is now orphaned and the caller should delete it: owner-bind owner left, or
// no members remain. owner-migration hands ownership to the next remaining member.
bool RemoveMemberApplyOwner(SharedState::Session& sn, int64_t userId, bool* outRemoved) {
    const bool wasOwner = (sn.ownerUserId == userId);
    bool removed = false;
    for (int m = 0; m < sn.members.size(); ++m) {
        if (sn.members[m].userId == userId) {
            sn.members.removeAt(m);
            removed = true;
            break;
        }
    }
    if (outRemoved)
        *outRemoved = removed;
    if (!removed)
        return false;
    if ((sn.sessionType == QStringLiteral("owner-bind") && wasOwner) || sn.members.isEmpty())
        return true;
    if (wasOwner) {
        sn.ownerUserId = sn.members.first().userId;
        sn.ownerNpid = sn.members.first().npid;
    }
    return false;
}

QHttpServerResponse HandleSessionLeave(Database& db, SharedState& shared, const QString& sessionId,
                                       const QString& memberKey, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    int64_t targetId = 0;
    if (!ResolveTarget(db, memberKey, auth, targetId)) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent}; // nothing to remove
    }

    QWriteLocker lk(&shared.sessionsLock);
    auto it = shared.sessions.find(sessionId);
    if (it == shared.sessions.end()) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent}; // already not in it
    }
    auto& sn = it.value();
    // Self-leave is always allowed; removing a different member requires being the owner.
    if (targetId != *auth.userId && sn.ownerUserId != *auth.userId) {
        return JsonError(QHttpServerResponse::StatusCode::Forbidden, SESSION_NOT_PERMITTED,
                         QStringLiteral("Not permitted to access the session"));
    }
    bool removed = false;
    const bool shouldDelete = RemoveMemberApplyOwner(sn, targetId, &removed);
    if (!removed) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent}; // wasn't a member
    }
    if (shouldDelete)
        shared.sessions.remove(sessionId);
    qInfo() << "WebAPI: session leave" << sessionId << "member" << targetId << "by" << auth.npid;
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

// POST /v1/sessions/<sessionId>/invitations -- send an invitation linked to a session.
// multipart/mixed: required 'invitation-request' JSON part ({to:[accountId...], message})
// plus an optional 'invitation-data' octet-stream part (<=1 MiB). One Invitation record is
// stored per recipient; 204 on success (no body -- ids are delivered to recipients, not the
// sender). The session must exist. Per the spec the sender need not be a member, so that is
// not enforced here.
QHttpServerResponse HandleSessionInvite(Database& db, SharedState& shared, const QString& sessionId,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }

    const QByteArray boundary = ExtractBoundary(req.value("Content-Type"));
    if (boundary.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Expected multipart/mixed body with a boundary"));
    }
    const QList<MultipartPart> parts = ParseMultipartMixed(req.body(), boundary);

    QByteArray jsonPart, dataPart;
    bool haveJson = false, haveData = false;
    for (const auto& p : parts) {
        const QByteArray& d = p.contentDescription;
        if (d == "invitation-request") {
            jsonPart = p.data;
            haveJson = true;
        } else if (d == "invitation-data") {
            dataPart = p.data;
            haveData = true;
        }
    }
    if (!haveJson) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_BODY_PARAM_REQUIRED,
                         QStringLiteral("Missing required invitation-request part"));
    }
    if (dataPart.size() > kInvitationDataMax) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_TOO_LARGE_BODY,
                         QStringLiteral("invitation-data exceeds the 1 MiB limit"));
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPart, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_BODY,
                         QStringLiteral("Malformed invitation-request JSON"));
    }
    const QJsonObject obj = doc.object();
    const QString message = obj.value(QStringLiteral("message")).toString();

    // 'to' is a list of target account IDs (numeric == userId in shadNet). Resolve each to a
    // (userId, npid) pair before taking the lock; unparseable/zero ids are skipped.
    QList<QPair<int64_t, QString>> recipients;
    for (const auto& v : obj.value(QStringLiteral("to")).toArray()) {
        bool ok = false;
        const int64_t uid = v.toString().toLongLong(&ok);
        if (!ok || uid == 0) {
            continue;
        }
        QString npid;
        if (auto n = db.GetUsername(uid)) {
            npid = *n;
        }
        recipients.append({uid, npid});
    }
    if (recipients.isEmpty()) {
        return JsonError(
            QHttpServerResponse::StatusCode::BadRequest, WEBAPI_INVALID_NUM_ELEMS,
            QStringLiteral("invitation-request 'to' must list at least one account ID"));
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QWriteLocker lk(&shared.sessionsLock);
        auto sit = shared.sessions.constFind(sessionId);
        if (sit == shared.sessions.constEnd()) {
            return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                             QStringLiteral("The session does not exist"));
        }
        const QStringList sessionPlatforms = sit->availablePlatforms;
        for (const auto& r : recipients) {
            SharedState::Invitation inv;
            inv.invitationId =
                QStringLiteral("002-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            inv.sessionId = sessionId;
            inv.availablePlatforms = sessionPlatforms;
            inv.fromUserId = *auth.userId;
            inv.fromNpid = auth.npid;
            inv.toUserId = r.first;
            inv.toNpid = r.second;
            inv.message = message;
            inv.invitationData = dataPart;
            inv.hasInvitationData = haveData;
            inv.createdAt = now;
            inv.used = false;
            shared.invitations.insert(inv.invitationId, inv);
        }
    }
    qInfo() << "WebAPI: session invite" << sessionId << "from" << auth.npid << "to"
            << recipients.size() << "recipient(s)" << (haveData ? "(with data)" : "");
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

// GET /v1/users/me/invitations/<invitationId> -- info on an invitation received by the caller.
// fields: @default (receivedDate, expired, usedFlag, message, availablePlatforms, fromUser); add
// `session` (or `session.sessionLockFlag`) for the linked session object, `members` for its
// member list. npLanguage localizes the session name/status. 200 application/json.
QHttpServerResponse HandleInvitationGet(Database& db, SharedState& shared, const QString& userKey,
                                        const QString& invitationId,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    Q_UNUSED(userKey); // path is /users/me/...; the bearer token identifies the recipient

    const QUrlQuery query(req.url());
    const QStringList fields =
        query.queryItemValue(QStringLiteral("fields")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool isDefault = fields.isEmpty() || fields.contains(QStringLiteral("@default"));
    const bool wantSession = fields.contains(QStringLiteral("session")) ||
                             fields.contains(QStringLiteral("session.sessionLockFlag"));
    const bool wantLockFlag = fields.contains(QStringLiteral("session.sessionLockFlag"));
    const bool wantMembers = fields.contains(QStringLiteral("members"));
    const QString npLanguage = query.queryItemValue(QStringLiteral("npLanguage"));

    QJsonObject body;
    {
        QReadLocker lk(&shared.sessionsLock);
        auto iit = shared.invitations.constFind(invitationId);
        if (iit == shared.invitations.constEnd() || iit->toUserId != *auth.userId) {
            return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                             QStringLiteral("The invitation does not exist"));
        }
        const auto& inv = iit.value();

        if (isDefault) {
            body.insert(QStringLiteral("receivedDate"),
                        QDateTime::fromMSecsSinceEpoch(inv.createdAt, QTimeZone::UTC)
                            .toString(Qt::ISODate));
            body.insert(QStringLiteral("expired"),
                        inv.validUntil != 0 &&
                            QDateTime::currentMSecsSinceEpoch() > inv.validUntil);
            body.insert(QStringLiteral("usedFlag"), inv.used);
            body.insert(QStringLiteral("message"), inv.message);
            QJsonArray plats;
            for (const auto& pf : inv.availablePlatforms)
                plats.append(pf);
            body.insert(QStringLiteral("availablePlatforms"), plats);
            QJsonObject fromUser;
            fromUser.insert(QStringLiteral("onlineId"), inv.fromNpid);
            fromUser.insert(QStringLiteral("accountId"), QString::number(inv.fromUserId));
            body.insert(QStringLiteral("fromUser"), fromUser);
        }

        // Session member: omitted if the linked session was deleted / no longer exists (per doc).
        if (wantSession) {
            auto ssit = shared.sessions.constFind(inv.sessionId);
            if (ssit != shared.sessions.constEnd()) {
                const auto& sn = ssit.value();
                QString name = sn.sessionName, status = sn.sessionStatus;
                if (!npLanguage.isEmpty()) {
                    const auto n = sn.localizedSessionNames.constFind(npLanguage);
                    if (n != sn.localizedSessionNames.constEnd())
                        name = n.value();
                    const auto st = sn.localizedSessionStatus.constFind(npLanguage);
                    if (st != sn.localizedSessionStatus.constEnd())
                        status = st.value();
                }
                QJsonObject so;
                so.insert(QStringLiteral("sessionId"), sn.sessionId);
                so.insert(QStringLiteral("sessionType"), sn.sessionType);
                so.insert(QStringLiteral("sessionPrivacy"), sn.sessionPrivacy);
                so.insert(QStringLiteral("sessionMaxUser"), sn.sessionMaxUser);
                so.insert(QStringLiteral("sessionName"), name);
                so.insert(QStringLiteral("sessionStatus"), status);
                QJsonObject creator;
                creator.insert(QStringLiteral("onlineId"), sn.ownerNpid);
                creator.insert(QStringLiteral("accountId"), QString::number(sn.ownerUserId));
                so.insert(QStringLiteral("sessionCreator"), creator);
                so.insert(QStringLiteral("sessionCreateTimestamp"), sn.createdAt);
                if (wantLockFlag)
                    so.insert(QStringLiteral("sessionLockFlag"), sn.sessionLockFlag);
                if (wantMembers) {
                    QJsonArray members;
                    for (const auto& m : sn.members) {
                        QJsonObject mo;
                        mo.insert(QStringLiteral("accountId"), QString::number(m.userId));
                        mo.insert(QStringLiteral("onlineId"), m.npid);
                        mo.insert(QStringLiteral("platform"), m.platform);
                        members.append(mo);
                    }
                    so.insert(QStringLiteral("members"), members);
                }
                body.insert(QStringLiteral("session"), so);
            }
        }
    }
    return JsonOk(body);
}

// GET /v1/users/me/invitations/<invitationId>/invitationData -- the invitation's binary data.
// Falls back to the linked session's sessionData, then changeableSessionData, when no invitation
// data was set. 200 application/octet-stream. Expired/used -> 2115584; not-found, wrong recipient,
// or a platform outside availablePlatforms -> 2113549.
QHttpServerResponse HandleInvitationData(Database& db, SharedState& shared, const QString& userKey,
                                         const QString& invitationId,
                                         const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    Q_UNUSED(userKey);

    // Caller's platform for the availablePlatforms gate (empty if offline).
    QString callerPlatform;
    {
        QReadLocker lk(&shared.clientsLock);
        auto it = shared.clients.constFind(*auth.userId);
        if (it != shared.clients.constEnd())
            callerPlatform = it->platform;
    }

    QByteArray data;
    {
        QReadLocker lk(&shared.sessionsLock);
        auto iit = shared.invitations.constFind(invitationId);
        if (iit == shared.invitations.constEnd() || iit->toUserId != *auth.userId) {
            return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                             QStringLiteral("The invitation does not exist"));
        }
        const auto& inv = iit.value();

        const bool expired =
            inv.validUntil != 0 && QDateTime::currentMSecsSinceEpoch() > inv.validUntil;
        if (expired || inv.used) {
            return JsonError(QHttpServerResponse::StatusCode::NotFound, INVITATION_EXPIRED,
                             QStringLiteral("Invitation is expired or data has been used"));
        }
        if (!callerPlatform.isEmpty() && !inv.availablePlatforms.isEmpty() &&
            !inv.availablePlatforms.contains(callerPlatform)) {
            return JsonError(QHttpServerResponse::StatusCode::NotFound, WEBAPI_RESOURCE_NOT_FOUND,
                             QStringLiteral("The platform cannot use this session"));
        }

        if (inv.hasInvitationData) {
            data = inv.invitationData;
        } else {
            // Fallback: session data, then changeable session data.
            auto ssit = shared.sessions.constFind(inv.sessionId);
            if (ssit == shared.sessions.constEnd()) {
                return JsonError(QHttpServerResponse::StatusCode::NotFound,
                                 WEBAPI_RESOURCE_NOT_FOUND,
                                 QStringLiteral("The linked session does not exist"));
            }
            data = !ssit->sessionData.isEmpty() ? ssit->sessionData : ssit->changeableSessionData;
        }
    }
    return QHttpServerResponse(QByteArrayLiteral("application/octet-stream"), data,
                               QHttpServerResponse::StatusCode::Ok);
}

} // namespace

void RegisterSessionRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // POST /v1/sessions -- create and join a session (multipart/mixed body).
    http.route("/v1/sessions", QHttpServerRequest::Method::Post,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionCreate(db, shared, req);
               });

    // POST /v1/sessions/<arg>/members -- join a session.
    http.route("/v1/sessions/<arg>/members", QHttpServerRequest::Method::Post,
               [&db, &shared](const QString& sessionId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionJoin(db, shared, sessionId, req);
               });

    // POST /v1/sessions/<arg>/invitations -- send an invitation linked to a session.
    http.route("/v1/sessions/<arg>/invitations", QHttpServerRequest::Method::Post,
               [&db, &shared](const QString& sessionId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionInvite(db, shared, sessionId, req);
               });

    // GET /v1/users/<arg>/invitations/<arg> -- info on a received invitation.
    http.route("/v1/users/<arg>/invitations/<arg>", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& userKey, const QString& invitationId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleInvitationGet(db, shared, userKey, invitationId, req);
               });

    // GET /v1/users/<arg>/invitations/<arg>/invitationData -- the invitation's binary data.
    http.route("/v1/users/<arg>/invitations/<arg>/invitationData", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& userKey, const QString& invitationId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleInvitationData(db, shared, userKey, invitationId, req);
               });

    // GET /v1/sessions/<arg>/sessionData -- the session's binary data blob.
    http.route("/v1/sessions/<arg>/sessionData", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& sessionId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionGetData(db, shared, sessionId, req);
               });

    // DELETE /v1/sessions/<arg>/members/<arg> -- leave a session.
    http.route("/v1/sessions/<arg>/members/<arg>", QHttpServerRequest::Method::Delete,
               [&db, &shared](const QString& sessionId, const QString& memberKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionLeave(db, shared, sessionId, memberKey, req);
               });

    // GET /v1/users/<arg>/sessions -- list a user's sessions.
    http.route("/v1/users/<arg>/sessions",
               [&db, &shared](const QString& userKey,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   if (userKey == QStringLiteral("@users"))
                       return HandleMultiUserSessions(db, shared, req);
                   return HandleSessionList(db, shared, userKey, req);
               });

    // GET /v1/sessions/<arg> -- session information.
    http.route("/v1/sessions/<arg>", QHttpServerRequest::Method::Get,
               [&db, &shared](const QString& sessionId, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleSessionGet(db, shared, sessionId, req); });

    // GET /v1/users/<arg>/friends/sessions -- sessions prioritized by the caller's friends.
    http.route("/v1/users/<arg>/friends/sessions",
               [&db, &shared](const QString& /*userKey*/, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleFriendsSessions(db, shared, req); });

    // DELETE /v1/sessions/<arg> -- delete a session.
    http.route("/v1/sessions/<arg>", QHttpServerRequest::Method::Delete,
               [&db, &shared](const QString& sessionId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionDelete(db, shared, sessionId, req);
               });

    // PUT /v1/sessions/<arg> -- update session information.
    http.route("/v1/sessions/<arg>", QHttpServerRequest::Method::Put,
               [&db, &shared](const QString& sessionId,
                              const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionUpdate(db, shared, sessionId, req);
               });
}

// Remove a user from every session they are in, applying owner-migration / owner-bind
// teardown. Called on disconnect so an offline user automatically leaves their sessions.
void PurgeUserFromSessions(SharedState& shared, int64_t userId) {
    QWriteLocker lk(&shared.sessionsLock);
    QStringList toRemove;
    for (auto it = shared.sessions.begin(); it != shared.sessions.end(); ++it) {
        if (RemoveMemberApplyOwner(it.value(), userId, nullptr))
            toRemove.append(it.key());
    }
    for (const QString& sid : toRemove)
        shared.sessions.remove(sid);
    if (!toRemove.isEmpty())
        qInfo() << "Sessions: purged user" << userId << "from" << toRemove.size()
                << "session(s) on disconnect";
}

} // namespace WebApiRoutes
