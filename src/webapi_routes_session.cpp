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
#include <QString>
#include <QStringList>
#include <QUuid>

#include "client_session.h" // SharedState
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

// Session/Invitation error codes. Only "user not online" is documented in the Session Creation
// spec; SESSION_BAD_REQUEST is a placeholder pending the group's common error-code table.
constexpr quint32 SESSION_USER_NOT_ONLINE = 2114564;
constexpr quint32 SESSION_BAD_REQUEST = 2114561; // PLACEHOLDER (not from the doc)

constexpr int kImageMax = 160 * 1024;
constexpr int kDataMax = 1024 * 1024;
constexpr int kChangeableMax = 1024;

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

QHttpServerResponse HandleSessionCreate(Database& db, SharedState& shared,
                                        const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }

    const QByteArray boundary = ExtractBoundary(req.value("Content-Type"));
    if (boundary.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
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
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
                         QStringLiteral("Missing required multipart part"));
    }
    if (imagePart.size() > kImageMax || dataPart.size() > kDataMax ||
        changeablePart.size() > kChangeableMax) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
                         QStringLiteral("Multipart part exceeds the size limit"));
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPart, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
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
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
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
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, SESSION_BAD_REQUEST,
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

} // namespace

void RegisterSessionRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // POST /v1/sessions -- create and join a session (multipart/mixed body).
    http.route("/v1/sessions", QHttpServerRequest::Method::Post,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleSessionCreate(db, shared, req);
               });
}

} // namespace WebApiRoutes
