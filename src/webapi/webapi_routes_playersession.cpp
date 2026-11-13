// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_playersession.h"

#include <optional>
#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QUuid>

#include "client_session.h"
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

constexpr quint32 PS_RESOURCE_NOT_FOUND = 2113549;
constexpr quint32 PS_INVALID_BODY = 2113920;
constexpr quint32 PS_INVALID_NUM_ELEMS = 2113923;
constexpr quint32 PS_NOT_PERMITTED = 2114560;
constexpr quint32 PS_SESSION_FULL = 2114561;
constexpr quint32 PS_ONLY_LEADER = 2114562;
constexpr quint32 PS_ONLY_MEMBER = 2114563;

constexpr int kMaxSessions = 4096;
constexpr int kMaxPlayersLimit = 100;

enum class Event { MemberJoined, MemberLeft, Updated, LeaderChanged, Message, Invitation };

// The data type strings a title registers its push filters with are not known on the server side,
// so every event is announced as a '|' separated list of candidates. The emulator hands the event
// to the filter whose registered data type ends with one of the candidates (case and separators
// ignored). Once the real strings are known they can replace the lists here.
QString EventCandidates(Event e) {
    switch (e) {
    case Event::MemberJoined:
        return QStringLiteral("playerSession:member:joined|playerSession:player:joined|"
                              "playerSession:memberJoined|session:player:join:to:member");
    case Event::MemberLeft:
        return QStringLiteral("playerSession:member:left|playerSession:player:left|"
                              "playerSession:memberLeft|session:player:leave:to:member");
    case Event::Updated:
        return QStringLiteral(
            "playerSession:updated|playerSession:update|session:player:update:to:member");
    case Event::LeaderChanged:
        return QStringLiteral("playerSession:leader:changed|playerSession:leaderChanged|"
                              "playerSession:leader:updated");
    case Event::Message:
        return QStringLiteral("playerSession:sessionMessage|playerSession:message|"
                              "playerSession:messageReceived|playerSession:session:message");
    case Event::Invitation:
        return QStringLiteral("playerSession:invitation|playerSession:invitation:received|"
                              "playerSession:invite|invitation");
    }
    return {};
}

struct PsMember {
    int64_t userId = 0;
    QString npid;
    QString platform;
    QStringList pushContextIds;
    qint64 joinedAt = 0;
    bool spectator = false;
};

struct PsSession {
    QString sessionId;
    qint64 createdAt = 0;
    int64_t leaderUserId = 0;
    QStringList supportedPlatforms;
    int maxPlayers = 8;
    int maxSpectators = 0;
    bool joinDisabled = false;
    QString joinableUserType = QStringLiteral("ANYONE");
    QString invitableUserType = QStringLiteral("MEMBER");
    QStringList exclusiveLeaderPrivileges;
    bool swapSupported = false;
    QString customData1;
    QString customData2;
    QJsonObject localizedSessionName;
    QList<PsMember> members;
    QSet<int64_t> invitedUserIds;
};

struct Caller {
    int64_t userId = 0;
    QString npid;
    QString platform;
};

struct Outgoing {
    Event kind = Event::Updated;
    QString sessionId;
    int64_t fromUserId = 0;
    QString fromNpid;
    QByteArray body;
    int64_t toUserId = 0;
    QStringList toPushContexts;
};

// Own lock, independent from SharedState's locks. Never held while taking clientsLock: handlers
// collect the events to send under this lock, release it, and only then dispatch.
QMutex g_mutex;
QHash<QString, PsSession> g_sessions;

QString IsoTime(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODateWithMs);
}

QHttpServerResponse JsonResponse(const QJsonObject& body, QHttpServerResponse::StatusCode status) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

QHttpServerResponse NoContent() {
    return QHttpServerResponse{QHttpServerResponse::StatusCode::NoContent};
}

QHttpServerResponse NotFound(const QString& what) {
    return JsonError(QHttpServerResponse::StatusCode::NotFound, PS_RESOURCE_NOT_FOUND,
                     QStringLiteral("Resource not found (%1)").arg(what));
}

QHttpServerResponse InvalidBody() {
    return JsonError(QHttpServerResponse::StatusCode::BadRequest, PS_INVALID_BODY,
                     QStringLiteral("Invalid request body"));
}

QHttpServerResponse Forbidden(quint32 code, const QString& message) {
    return JsonError(QHttpServerResponse::StatusCode::Forbidden, code, message);
}

// An empty body parses to an empty object. Returns false for malformed JSON or a non-object.
bool ParseJsonObject(const QByteArray& body, QJsonObject& out) {
    out = QJsonObject{};
    if (body.trimmed().isEmpty()) {
        return true;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "WebAPI: playerSession body is not a JSON object:" << err.errorString()
                   << "body:" << body.left(512);
        return false;
    }
    out = doc.object();
    return true;
}

QStringList ToStringList(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& item : v.toArray()) {
        const QString s = item.toString();
        if (!s.isEmpty()) {
            out << s;
        }
    }
    return out;
}

// pushContexts: [{"pushContextId": "<uuid>"}, ...]
QStringList ParsePushContexts(const QJsonObject& player) {
    QStringList ids;
    for (const QJsonValue& v : player.value(QStringLiteral("pushContexts")).toArray()) {
        const QString id = v.isObject()
                               ? v.toObject().value(QStringLiteral("pushContextId")).toString()
                               : v.toString();
        if (!id.isEmpty() && !ids.contains(id)) {
            ids << id;
        }
    }
    return ids;
}

Caller MakeCaller(const WebApiAuth::AuthResult& auth, SharedState& shared) {
    Caller c;
    c.userId = *auth.userId;
    c.npid = auth.npid;
    c.platform = QStringLiteral("PS4");
    QReadLocker lk(&shared.clientsLock);
    const auto it = shared.clients.constFind(c.userId);
    if (it != shared.clients.constEnd()) {
        if (c.npid.isEmpty()) {
            c.npid = it->npid;
        }
        if (!it->platform.isEmpty()) {
            c.platform = it->platform;
        }
    }
    return c;
}

QSet<int64_t> OnlineFriendIds(SharedState& shared, int64_t userId) {
    QSet<int64_t> ids;
    QReadLocker lk(&shared.clientsLock);
    const auto it = shared.clients.constFind(userId);
    if (it != shared.clients.constEnd()) {
        for (auto f = it->friends.cbegin(); f != it->friends.cend(); ++f) {
            ids.insert(f.key());
        }
    }
    return ids;
}

// "me" | numeric account id | online id
std::optional<int64_t> ResolveAccount(Database& db, const QString& key, int64_t me) {
    const QString k = key.trimmed();
    if (k.isEmpty() || k.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0) {
        return me;
    }
    bool ok = false;
    const qint64 id = k.toLongLong(&ok);
    if (ok) {
        return id;
    }
    return db.GetUserId(k);
}

PsMember* FindMember(PsSession& s, int64_t userId) {
    for (PsMember& m : s.members) {
        if (m.userId == userId) {
            return &m;
        }
    }
    return nullptr;
}

const PsMember* FindMember(const PsSession& s, int64_t userId) {
    for (const PsMember& m : s.members) {
        if (m.userId == userId) {
            return &m;
        }
    }
    return nullptr;
}

int CountMembers(const PsSession& s, bool spectators) {
    int n = 0;
    for (const PsMember& m : s.members) {
        if (m.spectator == spectators) {
            ++n;
        }
    }
    return n;
}

QJsonObject MemberJson(const PsMember& m) {
    QJsonObject o;
    o.insert(QStringLiteral("accountId"), QString::number(m.userId));
    o.insert(QStringLiteral("onlineId"), m.npid);
    o.insert(QStringLiteral("platform"), m.platform);
    o.insert(QStringLiteral("joinTimestamp"), IsoTime(m.joinedAt));
    if (!m.pushContextIds.isEmpty()) {
        QJsonArray ctxs;
        for (const QString& id : m.pushContextIds) {
            QJsonObject c;
            c.insert(QStringLiteral("pushContextId"), id);
            ctxs.append(c);
        }
        o.insert(QStringLiteral("pushContexts"), ctxs);
    }
    return o;
}

QJsonObject SessionJson(const PsSession& s) {
    QJsonObject o;
    o.insert(QStringLiteral("sessionId"), s.sessionId);
    o.insert(QStringLiteral("createdTimestamp"), IsoTime(s.createdAt));
    o.insert(QStringLiteral("maxPlayers"), s.maxPlayers);
    o.insert(QStringLiteral("maxSpectators"), s.maxSpectators);
    o.insert(QStringLiteral("joinDisabled"), s.joinDisabled);
    o.insert(QStringLiteral("joinableUserType"), s.joinableUserType);
    o.insert(QStringLiteral("invitableUserType"), s.invitableUserType);
    o.insert(QStringLiteral("exclusiveLeaderPrivileges"),
             QJsonArray::fromStringList(s.exclusiveLeaderPrivileges));
    o.insert(QStringLiteral("supportedPlatforms"),
             QJsonArray::fromStringList(s.supportedPlatforms));
    o.insert(QStringLiteral("swapSupported"), s.swapSupported);
    if (!s.customData1.isEmpty()) {
        o.insert(QStringLiteral("customData1"), s.customData1);
    }
    if (!s.customData2.isEmpty()) {
        o.insert(QStringLiteral("customData2"), s.customData2);
    }
    if (!s.localizedSessionName.isEmpty()) {
        o.insert(QStringLiteral("localizedSessionName"), s.localizedSessionName);
    }
    QJsonArray players;
    QJsonArray spectators;
    for (const PsMember& m : s.members) {
        (m.spectator ? spectators : players).append(MemberJson(m));
        if (m.userId == s.leaderUserId) {
            QJsonObject leader;
            leader.insert(QStringLiteral("accountId"), QString::number(m.userId));
            leader.insert(QStringLiteral("platform"), m.platform);
            o.insert(QStringLiteral("leader"), leader);
        }
    }
    QJsonObject member;
    member.insert(QStringLiteral("players"), players);
    member.insert(QStringLiteral("spectators"), spectators);
    o.insert(QStringLiteral("member"), member);
    return o;
}

// Wrapped the way both a collection response and a single-resource response might be read.
QJsonObject SessionEnvelope(const PsSession& s) {
    const QJsonObject session = SessionJson(s);
    QJsonObject o;
    o.insert(QStringLiteral("playerSessions"), QJsonArray{session});
    o.insert(QStringLiteral("playerSession"), session);
    return o;
}

// Applies every setting present in `spec`; used for both create and update.
void ApplySettings(PsSession& s, const QJsonObject& spec) {
    using Key = QLatin1String;
    if (spec.contains(Key("supportedPlatforms"))) {
        s.supportedPlatforms = ToStringList(spec.value(Key("supportedPlatforms")));
    }
    if (spec.contains(Key("maxPlayers"))) {
        s.maxPlayers =
            qBound(1, spec.value(Key("maxPlayers")).toInt(s.maxPlayers), kMaxPlayersLimit);
    }
    if (spec.contains(Key("maxSpectators"))) {
        s.maxSpectators =
            qBound(0, spec.value(Key("maxSpectators")).toInt(s.maxSpectators), kMaxPlayersLimit);
    }
    if (spec.contains(Key("joinDisabled"))) {
        s.joinDisabled = spec.value(Key("joinDisabled")).toBool(s.joinDisabled);
    }
    if (spec.contains(Key("joinableUserType"))) {
        s.joinableUserType = spec.value(Key("joinableUserType")).toString(s.joinableUserType);
    }
    if (spec.contains(Key("invitableUserType"))) {
        s.invitableUserType = spec.value(Key("invitableUserType")).toString(s.invitableUserType);
    }
    if (spec.contains(Key("exclusiveLeaderPrivileges"))) {
        s.exclusiveLeaderPrivileges = ToStringList(spec.value(Key("exclusiveLeaderPrivileges")));
    }
    if (spec.contains(Key("swapSupported"))) {
        s.swapSupported = spec.value(Key("swapSupported")).toBool(s.swapSupported);
    }
    if (spec.contains(Key("customData1"))) {
        s.customData1 = spec.value(Key("customData1")).toString();
    }
    if (spec.contains(Key("customData2"))) {
        s.customData2 = spec.value(Key("customData2")).toString();
    }
    if (spec.contains(Key("localizedSessionName"))) {
        s.localizedSessionName = spec.value(Key("localizedSessionName")).toObject();
    }
}

// The session object may arrive bare or wrapped in {"playerSessions": [ ... ]}.
bool UnwrapSessionSpec(const QJsonObject& root, QJsonObject& spec) {
    spec = root;
    if (root.contains(QLatin1String("playerSessions"))) {
        const QJsonArray arr = root.value(QLatin1String("playerSessions")).toArray();
        if (arr.size() != 1) {
            return false;
        }
        spec = arr.at(0).toObject();
    }
    return true;
}

void QueueEvent(QList<Outgoing>& out, const PsSession& s, Event kind, const QJsonObject& body,
                int64_t fromUserId, const QString& fromNpid, int64_t exceptUserId = -1,
                const QSet<int64_t>* onlyTo = nullptr) {
    const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    for (const PsMember& m : s.members) {
        if (m.userId == exceptUserId || (onlyTo && !onlyTo->contains(m.userId))) {
            continue;
        }
        Outgoing ev;
        ev.kind = kind;
        ev.sessionId = s.sessionId;
        ev.fromUserId = fromUserId;
        ev.fromNpid = fromNpid;
        ev.body = bytes;
        ev.toUserId = m.userId;
        ev.toPushContexts = m.pushContextIds;
        out.append(ev);
    }
}

// Sends each event as a WebApiPushEvent notification to the recipient's emulator. A recipient
// with several push contexts gets one packet per context so the emulator can route it to the
// right context callback. Acquires clientsLock only.
void Dispatch(SharedState& shared, const QList<Outgoing>& events) {
    if (events.isEmpty()) {
        return;
    }
    QReadLocker lk(&shared.clientsLock);
    for (const Outgoing& ev : events) {
        const auto it = shared.clients.constFind(ev.toUserId);
        if (it == shared.clients.constEnd() || !it->send) {
            continue;
        }
        const QStringList contexts =
            ev.toPushContexts.isEmpty() ? QStringList{QString()} : ev.toPushContexts;
        for (const QString& ctx : contexts) {
            QList<QPair<QString, QString>> extd;
            extd.append({QStringLiteral("sessionId"), ev.sessionId});
            extd.append({QStringLiteral("fromAccountId"), QString::number(ev.fromUserId)});
            extd.append({QStringLiteral("toAccountId"), QString::number(ev.toUserId)});
            if (!ctx.isEmpty()) {
                extd.append({QStringLiteral("pushContextId"), ctx});
            }
            // The emulator raises its accept prompt only for service "sessionInvitation" and
            // reads the invitation id from extd, as the legacy session invitations provide.
            QString service;
            if (ev.kind == Event::Invitation) {
                service = QStringLiteral("sessionInvitation");
                const QString invitationId = QJsonDocument::fromJson(ev.body)
                                                 .object()
                                                 .value(QStringLiteral("invitationId"))
                                                 .toString();
                extd.append({QStringLiteral("invitationId"), invitationId});
            }
            it->send(ClientSession::BuildNotification(
                NotificationType::WebApiPushEvent,
                ClientSession::BuildWebApiPushPayload(service, 0, EventCandidates(ev.kind),
                                                      ev.body, ev.fromNpid, it->npid, extd,
                                                      ev.fromUserId, ev.toUserId)));
        }
        qInfo() << "WebAPI: playerSession event" << EventCandidates(ev.kind).section('|', 0, 0)
                << "session" << ev.sessionId << "-> account" << ev.toUserId;
    }
}

// Removes a member (caller holds g_mutex), migrating leadership and queueing the notifications.
// Returns false when the user is not a member.
bool RemoveMember(PsSession& s, int64_t userId, const QString& reason, QList<Outgoing>& events) {
    int idx = -1;
    for (int i = 0; i < s.members.size(); ++i) {
        if (s.members.at(i).userId == userId) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return false;
    }
    const PsMember removed = s.members.takeAt(idx);

    QJsonObject body;
    body.insert(QStringLiteral("sessionId"), s.sessionId);
    body.insert(QStringLiteral("member"), MemberJson(removed));
    body.insert(QStringLiteral("reason"), reason);
    QueueEvent(events, s, Event::MemberLeft, body, removed.userId, removed.npid);
    if (reason == QLatin1String("kicked")) {
        Outgoing ev;
        ev.kind = Event::MemberLeft;
        ev.sessionId = s.sessionId;
        ev.fromUserId = removed.userId;
        ev.fromNpid = removed.npid;
        ev.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        ev.toUserId = removed.userId;
        ev.toPushContexts = removed.pushContextIds;
        events.append(ev);
    }

    if (s.leaderUserId == userId && CountMembers(s, false) > 0) {
        const PsMember* next = nullptr;
        for (const PsMember& m : s.members) {
            if (!m.spectator && (!next || m.joinedAt < next->joinedAt)) {
                next = &m;
            }
        }
        if (next) {
            s.leaderUserId = next->userId;
            QJsonObject leaderBody;
            leaderBody.insert(QStringLiteral("sessionId"), s.sessionId);
            QJsonObject leader;
            leader.insert(QStringLiteral("accountId"), QString::number(next->userId));
            leader.insert(QStringLiteral("platform"), next->platform);
            leaderBody.insert(QStringLiteral("leader"), leader);
            QueueEvent(events, s, Event::LeaderChanged, leaderBody, next->userId, next->npid);
        }
    }
    return true;
}

// 0 == permitted.
quint32 JoinPermission(const PsSession& s, int64_t userId, bool spectator,
                       const QSet<int64_t>& myFriends) {
    if (s.joinDisabled) {
        return PS_NOT_PERMITTED;
    }
    const int cap = spectator ? s.maxSpectators : s.maxPlayers;
    if (CountMembers(s, spectator) >= cap) {
        return PS_SESSION_FULL;
    }
    if (s.invitedUserIds.contains(userId)) {
        return 0;
    }
    const QString& type = s.joinableUserType;
    if (type == QLatin1String("ANYONE")) {
        return 0;
    }
    if (type == QLatin1String("FRIENDS") || type == QLatin1String("FRIENDS_OF_FRIENDS")) {
        for (const PsMember& m : s.members) {
            if (myFriends.contains(m.userId)) {
                return 0;
            }
        }
    }
    return PS_NOT_PERMITTED;
}

bool IsFriendJoinable(const PsSession& s, int64_t userId, const QSet<int64_t>& myFriends) {
    return JoinPermission(s, userId, false, myFriends) == 0;
}

QHttpServerResponse HandleCreate(Database& db, SharedState& shared, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    if (!ParseJsonObject(req.body(), root)) {
        return InvalidBody();
    }
    QJsonObject spec;
    if (!UnwrapSessionSpec(root, spec)) {
        return JsonError(
            QHttpServerResponse::StatusCode::BadRequest, PS_INVALID_NUM_ELEMS,
            QStringLiteral("Invalid number of elements in the array (playerSessions)"));
    }

    PsSession s;
    s.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.createdAt = QDateTime::currentMSecsSinceEpoch();
    s.leaderUserId = me.userId;
    s.supportedPlatforms = QStringList{me.platform};
    ApplySettings(s, spec);

    PsMember self;
    self.userId = me.userId;
    self.npid = me.npid;
    self.platform = me.platform;
    self.joinedAt = s.createdAt;
    const QJsonArray declared =
        spec.value(QStringLiteral("member")).toObject().value(QStringLiteral("players")).toArray();
    for (const QJsonValue& v : declared) {
        const QJsonObject p = v.toObject();
        const QString accountId = p.value(QStringLiteral("accountId")).toString();
        if (accountId.isEmpty() ||
            accountId.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
            accountId == QString::number(me.userId)) {
            self.pushContextIds = ParsePushContexts(p);
            const QString platform = p.value(QStringLiteral("platform")).toString();
            if (!platform.isEmpty()) {
                self.platform = platform;
            }
            break;
        }
    }
    s.members.append(self);

    QJsonObject response;
    {
        QMutexLocker lk(&g_mutex);
        if (g_sessions.size() >= kMaxSessions) {
            return JsonError(QHttpServerResponse::StatusCode::ServiceUnavailable, PS_NOT_PERMITTED,
                             QStringLiteral("Too many player sessions"));
        }
        g_sessions.insert(s.sessionId, s);
        response = SessionEnvelope(s);
    }
    qInfo() << "WebAPI: playerSession created" << s.sessionId << "by" << me.npid << "max"
            << s.maxPlayers << "joinable" << s.joinableUserType << "pushContexts"
            << self.pushContextIds;
    return JsonResponse(response, QHttpServerResponse::StatusCode::Created);
}

QHttpServerResponse HandleGetOne(Database& db, SharedState& shared, const QString& sessionId,
                                 const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const int64_t me = *auth.userId;
    Q_UNUSED(shared);

    QMutexLocker lk(&g_mutex);
    const auto it = g_sessions.constFind(sessionId);
    if (it == g_sessions.constEnd()) {
        return NotFound(sessionId);
    }
    const PsSession& s = it.value();
    if (s.joinableUserType == QLatin1String("NO_ONE") && !FindMember(s, me) &&
        !s.invitedUserIds.contains(me)) {
        return Forbidden(PS_NOT_PERMITTED,
                         QStringLiteral("Not permitted to view this player session"));
    }
    return JsonOk(SessionEnvelope(s));
}

// GET /v1/playerSessions?playerSessionIds=a,b (sessionIds is accepted as well)
QHttpServerResponse HandleList(Database& db, SharedState& shared, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const int64_t me = *auth.userId;
    Q_UNUSED(shared);

    const QUrlQuery query(req.url());
    QStringList ids;
    for (const char* key : {"playerSessionIds", "sessionIds", "sessionId"}) {
        ids << query.queryItemValue(QLatin1String(key)).split(QLatin1Char(','), Qt::SkipEmptyParts);
    }

    QJsonArray sessions;
    {
        QMutexLocker lk(&g_mutex);
        for (const QString& id : ids) {
            const auto it = g_sessions.constFind(id);
            if (it == g_sessions.constEnd()) {
                continue;
            }
            if (it->joinableUserType == QLatin1String("NO_ONE") && !FindMember(*it, me) &&
                !it->invitedUserIds.contains(me)) {
                continue;
            }
            sessions.append(SessionJson(*it));
        }
    }
    QJsonObject body;
    body.insert(QStringLiteral("playerSessions"), sessions);
    return JsonOk(body);
}

// GET /v1/users/<arg>/playerSessions -- the sessions a user is currently in.
QHttpServerResponse HandleUserSessions(Database& db, SharedState& shared, const QString& userKey,
                                       const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const int64_t me = *auth.userId;
    const auto target = ResolveAccount(db, userKey, me);
    if (!target) {
        return NotFound(userKey);
    }
    const bool self = *target == me;
    const QSet<int64_t> myFriends = self ? QSet<int64_t>{} : OnlineFriendIds(shared, me);

    QJsonArray sessions;
    {
        QMutexLocker lk(&g_mutex);
        for (auto it = g_sessions.cbegin(); it != g_sessions.cend(); ++it) {
            if (!FindMember(*it, *target)) {
                continue;
            }
            if (!self && !FindMember(*it, me) && !IsFriendJoinable(*it, me, myFriends)) {
                continue;
            }
            sessions.append(SessionJson(*it));
        }
    }
    QJsonObject body;
    body.insert(QStringLiteral("playerSessions"), sessions);
    return JsonOk(body);
}

// PATCH|PUT /v1/playerSessions/<arg>
QHttpServerResponse HandleUpdate(Database& db, SharedState& shared, const QString& sessionId,
                                 const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    if (!ParseJsonObject(req.body(), root)) {
        return InvalidBody();
    }
    QJsonObject spec;
    if (!UnwrapSessionSpec(root, spec)) {
        return InvalidBody();
    }

    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        if (!FindMember(s, me.userId)) {
            return Forbidden(PS_ONLY_MEMBER, QStringLiteral("Only session members are permitted to "
                                                            "perform this operation"));
        }
        ApplySettings(s, spec);
        QJsonObject body;
        body.insert(QStringLiteral("sessionId"), s.sessionId);
        body.insert(QStringLiteral("playerSession"), SessionJson(s));
        QueueEvent(events, s, Event::Updated, body, me.userId, me.npid);
    }
    qInfo() << "WebAPI: playerSession updated" << sessionId << "by" << me.npid;
    Dispatch(shared, events);
    return NoContent();
}

QHttpServerResponse HandleCustomData(Database& db, SharedState& shared, const QString& sessionId,
                                     const QString& field, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    // Either a JSON object carrying the field, or the raw binary payload.
    QString value;
    const QByteArray body = req.body();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject() && doc.object().contains(field)) {
        value = doc.object().value(field).toString();
    } else {
        value = QString::fromLatin1(body.toBase64());
    }

    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        if (!FindMember(s, me.userId)) {
            return Forbidden(PS_ONLY_MEMBER, QStringLiteral("Only session members are permitted to "
                                                            "perform this operation"));
        }
        (field == QLatin1String("customData1") ? s.customData1 : s.customData2) = value;
        QJsonObject eventBody;
        eventBody.insert(QStringLiteral("sessionId"), s.sessionId);
        eventBody.insert(QStringLiteral("changed"), QJsonArray{field});
        QueueEvent(events, s, Event::Updated, eventBody, me.userId, me.npid, me.userId);
    }
    Dispatch(shared, events);
    return NoContent();
}

// POST /v1/playerSessions/<arg>/member/players | /member/spectators
QHttpServerResponse HandleJoin(Database& db, SharedState& shared, const QString& sessionId,
                               bool spectator, const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    if (!ParseJsonObject(req.body(), root)) {
        return InvalidBody();
    }
    QStringList pushContexts;
    QString platform = me.platform;
    const QJsonArray declared =
        root.value(spectator ? QStringLiteral("spectators") : QStringLiteral("players")).toArray();
    for (const QJsonValue& v : declared) {
        const QJsonObject p = v.toObject();
        const QString accountId = p.value(QStringLiteral("accountId")).toString();
        if (accountId.isEmpty() ||
            accountId.compare(QStringLiteral("me"), Qt::CaseInsensitive) == 0 ||
            accountId == QString::number(me.userId)) {
            pushContexts = ParsePushContexts(p);
            if (!p.value(QStringLiteral("platform")).toString().isEmpty()) {
                platform = p.value(QStringLiteral("platform")).toString();
            }
            break;
        }
    }

    const QSet<int64_t> myFriends = OnlineFriendIds(shared, me.userId);

    QList<Outgoing> events;
    QJsonObject response;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        PsMember* existing = FindMember(s, me.userId);
        if (existing) {
            // Rejoining (for instance after the title recreated its push context) only refreshes
            // the push contexts.
            for (const QString& id : pushContexts) {
                if (!existing->pushContextIds.contains(id)) {
                    existing->pushContextIds << id;
                }
            }
        } else {
            const quint32 denied = JoinPermission(s, me.userId, spectator, myFriends);
            if (denied != 0) {
                return Forbidden(denied, denied == PS_SESSION_FULL
                                             ? QStringLiteral("The player session is full")
                                             : QStringLiteral("Not permitted to join this player "
                                                              "session"));
            }
            PsMember m;
            m.userId = me.userId;
            m.npid = me.npid;
            m.platform = platform;
            m.pushContextIds = pushContexts;
            m.joinedAt = QDateTime::currentMSecsSinceEpoch();
            m.spectator = spectator;
            s.members.append(m);
            s.invitedUserIds.remove(me.userId);

            QJsonObject body;
            body.insert(QStringLiteral("sessionId"), s.sessionId);
            body.insert(QStringLiteral("member"), MemberJson(m));
            QueueEvent(events, s, Event::MemberJoined, body, me.userId, me.npid, me.userId);
        }
        response = SessionEnvelope(s);
        if (const PsMember* mine = FindMember(s, me.userId)) {
            response.insert(QStringLiteral("member"), MemberJson(*mine));
        }
    }
    qInfo() << "WebAPI: playerSession" << sessionId << (spectator ? "spectator" : "player")
            << "joined:" << me.npid << "pushContexts" << pushContexts;
    Dispatch(shared, events);
    return JsonOk(response);
}

// DELETE /v1/playerSessions/<arg>/member/players[/<arg>] | /member/spectators[/<arg>]
// Without an explicit account (path segment or ?accountIds=) the caller leaves.
QHttpServerResponse HandleLeave(Database& db, SharedState& shared, const QString& sessionId,
                                bool spectator, const QString& pathAccount,
                                const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QStringList keys;
    if (!pathAccount.isEmpty()) {
        keys << pathAccount;
    }
    keys << QUrlQuery(req.url())
                .queryItemValue(QStringLiteral("accountIds"))
                .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (keys.isEmpty()) {
        keys << QStringLiteral("me");
    }
    QSet<int64_t> targets;
    for (const QString& key : keys) {
        const auto id = ResolveAccount(db, key, me.userId);
        if (id) {
            targets.insert(*id);
        }
    }

    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        for (const int64_t target : targets) {
            const PsMember* member = FindMember(s, target);
            if (!member || member->spectator != spectator) {
                continue;
            }
            if (target != me.userId && s.leaderUserId != me.userId) {
                return Forbidden(PS_ONLY_LEADER, QStringLiteral("Only the session leader is "
                                                                "permitted to remove other "
                                                                "members"));
            }
            RemoveMember(s, target,
                         target == me.userId ? QStringLiteral("left") : QStringLiteral("kicked"),
                         events);
        }
        if (CountMembers(s, false) == 0) {
            qInfo() << "WebAPI: playerSession" << sessionId << "closed (no players left)";
            g_sessions.erase(it);
        }
    }
    qInfo() << "WebAPI: playerSession" << sessionId << "member removal by" << me.npid;
    Dispatch(shared, events);
    return NoContent();
}

// POST /v1/playerSessions/<arg>/invitations
QHttpServerResponse HandleInvite(Database& db, SharedState& shared, const QString& sessionId,
                                 const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    if (!ParseJsonObject(req.body(), root)) {
        return InvalidBody();
    }
    // {"invitations":[{"to":{"accountId":".."}}]}, {"to":[{"accountId":".."}]} or
    // {"accountIds":[".."]}
    QStringList keys;
    const auto addKey = [&keys](const QJsonValue& v) {
        const QString k = v.isObject() ? v.toObject().value(QStringLiteral("accountId")).toString()
                                       : v.toString();
        if (!k.isEmpty()) {
            keys << k;
        }
    };
    for (const QJsonValue& inv : root.value(QStringLiteral("invitations")).toArray()) {
        const QJsonValue to = inv.toObject().value(QStringLiteral("to"));
        if (to.isArray()) {
            for (const QJsonValue& t : to.toArray()) {
                addKey(t);
            }
        } else {
            addKey(to);
        }
    }
    const QJsonValue toTop = root.value(QStringLiteral("to"));
    if (toTop.isArray()) {
        for (const QJsonValue& t : toTop.toArray()) {
            addKey(t);
        }
    } else if (!toTop.isUndefined()) {
        addKey(toTop);
    }
    for (const QJsonValue& v : root.value(QStringLiteral("accountIds")).toArray()) {
        addKey(v);
    }
    QList<int64_t> recipients;
    for (const QString& key : keys) {
        const auto id = ResolveAccount(db, key, me.userId);
        if (id && *id != me.userId && !recipients.contains(*id)) {
            recipients.append(*id);
        }
    }
    if (recipients.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::BadRequest, PS_INVALID_BODY,
                         QStringLiteral("No valid invitation recipient"));
    }

    QJsonArray invitations;
    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        if (!FindMember(s, me.userId)) {
            return Forbidden(PS_ONLY_MEMBER, QStringLiteral("Only session members are permitted to "
                                                            "perform this operation"));
        }
        if (s.invitableUserType == QLatin1String("NO_ONE") ||
            (s.invitableUserType == QLatin1String("LEADER") && s.leaderUserId != me.userId)) {
            return Forbidden(PS_NOT_PERMITTED,
                             QStringLiteral("Not permitted to invite to this player session"));
        }
        for (const int64_t to : recipients) {
            s.invitedUserIds.insert(to);
            const QString invitationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QJsonObject entry;
            entry.insert(QStringLiteral("invitationId"), invitationId);
            QJsonObject toObj;
            toObj.insert(QStringLiteral("accountId"), QString::number(to));
            entry.insert(QStringLiteral("to"), toObj);
            invitations.append(entry);

            QJsonObject body;
            body.insert(QStringLiteral("sessionId"), s.sessionId);
            body.insert(QStringLiteral("invitationId"), invitationId);
            body.insert(QStringLiteral("receivedDate"),
                        IsoTime(QDateTime::currentMSecsSinceEpoch()));
            QJsonObject fromUser;
            fromUser.insert(QStringLiteral("accountId"), QString::number(me.userId));
            fromUser.insert(QStringLiteral("onlineId"), me.npid);
            body.insert(QStringLiteral("fromUser"), fromUser);
            Outgoing ev;
            ev.kind = Event::Invitation;
            ev.sessionId = s.sessionId;
            ev.fromUserId = me.userId;
            ev.fromNpid = me.npid;
            ev.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
            ev.toUserId = to;
            events.append(ev);
        }
    }
    qInfo() << "WebAPI: playerSession" << sessionId << "invitations from" << me.npid << "to"
            << recipients;
    Dispatch(shared, events);
    QJsonObject body;
    body.insert(QStringLiteral("invitations"), invitations);
    return JsonOk(body);
}

// Sessions the user is currently a member of.
QStringList SessionsOfUser(int64_t userId) {
    QStringList ids;
    QMutexLocker lk(&g_mutex);
    for (auto it = g_sessions.cbegin(); it != g_sessions.cend(); ++it) {
        if (FindMember(it.value(), userId)) {
            ids << it.key();
        }
    }
    return ids;
}

// POST /v1/playerSessions/<arg>/sessionMessage
QHttpServerResponse HandleMessage(Database& db, SharedState& shared, const QString& sessionId,
                                  const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    QString payload;
    QSet<int64_t> only;
    bool restricted = false;
    if (ParseJsonObject(req.body(), root) && !req.body().trimmed().isEmpty()) {
        payload = root.value(QStringLiteral("payload")).toString();
        const QJsonValue to = root.value(QStringLiteral("to"));
        QJsonArray list;
        if (to.isArray()) {
            list = to.toArray();
        } else if (to.isObject()) {
            list.append(to);
        }
        for (const QJsonValue& v : list) {
            const QString key = v.isObject()
                                    ? v.toObject().value(QStringLiteral("accountId")).toString()
                                    : v.toString();
            const auto id = ResolveAccount(db, key, me.userId);
            if (id) {
                only.insert(*id);
            }
        }
        restricted = !list.isEmpty();
    } else {
        payload = QString::fromUtf8(req.body());
    }

    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.constFind(sessionId);
        if (it == g_sessions.constEnd()) {
            return NotFound(sessionId);
        }
        const PsSession& s = it.value();
        if (!FindMember(s, me.userId)) {
            return Forbidden(PS_ONLY_MEMBER, QStringLiteral("Only session members are permitted to "
                                                            "perform this operation"));
        }
        QJsonObject body;
        body.insert(QStringLiteral("sessionId"), s.sessionId);
        body.insert(QStringLiteral("payload"), payload);
        QJsonObject from;
        from.insert(QStringLiteral("accountId"), QString::number(me.userId));
        from.insert(QStringLiteral("onlineId"), me.npid);
        body.insert(QStringLiteral("from"), from);
        QueueEvent(events, s, Event::Message, body, me.userId, me.npid, me.userId,
                   restricted ? &only : nullptr);
    }
    qInfo() << "WebAPI: playerSession" << sessionId << "message from" << me.npid << "to"
            << events.size() << "member(s)";
    Dispatch(shared, events);
    return NoContent();
}

// PUT /v1/playerSessions/<arg>/leader
QHttpServerResponse HandleLeader(Database& db, SharedState& shared, const QString& sessionId,
                                 const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const Caller me = MakeCaller(auth, shared);

    QJsonObject root;
    if (!ParseJsonObject(req.body(), root)) {
        return InvalidBody();
    }
    const QString key = root.value(QStringLiteral("accountId")).toString();
    const auto target = ResolveAccount(db, key, me.userId);
    if (!target) {
        return InvalidBody();
    }

    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        const auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return NotFound(sessionId);
        }
        PsSession& s = it.value();
        if (s.leaderUserId != me.userId) {
            return Forbidden(PS_ONLY_LEADER, QStringLiteral("Only the session leader is permitted "
                                                            "to perform this operation"));
        }
        const PsMember* next = FindMember(s, *target);
        if (!next || next->spectator) {
            return Forbidden(PS_NOT_PERMITTED,
                             QStringLiteral("The new leader must be a player in the session"));
        }
        s.leaderUserId = *target;
        QJsonObject body;
        body.insert(QStringLiteral("sessionId"), s.sessionId);
        QJsonObject leader;
        leader.insert(QStringLiteral("accountId"), QString::number(*target));
        leader.insert(QStringLiteral("platform"), next->platform);
        body.insert(QStringLiteral("leader"), leader);
        QueueEvent(events, s, Event::LeaderChanged, body, me.userId, me.npid);
    }
    Dispatch(shared, events);
    return NoContent();
}

// GET /v1/friends/playerSessions -- joinable sessions that online friends are in. The response
// carries the sessions as a flat list and grouped per friend.
QHttpServerResponse HandleFriendsSessions(Database& db, SharedState& shared,
                                          const QHttpServerRequest& req) {
    auto auth = WebApiAuth::Authenticate(req, db);
    if (!auth.userId.has_value()) {
        return std::move(auth.errorResponse);
    }
    const int64_t me = *auth.userId;
    const QSet<int64_t> myFriends = OnlineFriendIds(shared, me);

    QJsonArray sessions;
    QHash<int64_t, QJsonArray> perFriend;
    {
        QMutexLocker lk(&g_mutex);
        for (auto it = g_sessions.cbegin(); it != g_sessions.cend(); ++it) {
            const PsSession& s = *it;
            if (FindMember(s, me) || !IsFriendJoinable(s, me, myFriends)) {
                continue;
            }
            QJsonArray friendIds;
            for (const PsMember& m : s.members) {
                if (myFriends.contains(m.userId)) {
                    friendIds.append(QString::number(m.userId));
                    QJsonObject ref;
                    ref.insert(QStringLiteral("sessionId"), s.sessionId);
                    perFriend[m.userId].append(ref);
                }
            }
            if (friendIds.isEmpty() && !s.invitedUserIds.contains(me)) {
                continue;
            }
            QJsonObject entry = SessionJson(s);
            entry.insert(QStringLiteral("friendAccountIds"), friendIds);
            sessions.append(entry);
        }
    }
    QJsonArray friends;
    for (auto it = perFriend.cbegin(); it != perFriend.cend(); ++it) {
        QJsonObject f;
        f.insert(QStringLiteral("accountId"), QString::number(it.key()));
        f.insert(QStringLiteral("playerSessions"), it.value());
        friends.append(f);
    }
    QJsonObject body;
    body.insert(QStringLiteral("playerSessions"), sessions);
    body.insert(QStringLiteral("friends"), friends);
    body.insert(QStringLiteral("totalItemCount"), sessions.size());
    return JsonOk(body);
}

} // namespace

void RegisterPlayerSessionRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    using Method = QHttpServerRequest::Method;

    http.route("/v1/playerSessions", Method::Post,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleCreate(db, shared, req);
               });
    http.route("/v1/playerSessions", Method::Get,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleList(db, shared, req);
               });
    http.route("/v1/playerSessions/<arg>", Method::Get,
               [&db, &shared](const QString& sessionId, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleGetOne(db, shared, sessionId, req); });
    const auto updateHandler = [&db,
                                &shared](const QString& sessionId,
                                         const QHttpServerRequest& req) -> QHttpServerResponse {
        return HandleUpdate(db, shared, sessionId, req);
    };
    http.route("/v1/playerSessions/<arg>", Method::Patch, updateHandler);
    http.route("/v1/playerSessions/<arg>", Method::Put, updateHandler);

    for (const bool spectator : {false, true}) {
        const QString group = spectator ? QStringLiteral("spectators") : QStringLiteral("players");
        http.route(QStringLiteral("/v1/playerSessions/<arg>/member/%1").arg(group), Method::Post,
                   [&db, &shared, spectator](const QString& sessionId,
                                             const QHttpServerRequest& req) -> QHttpServerResponse {
                       return HandleJoin(db, shared, sessionId, spectator, req);
                   });
        http.route(QStringLiteral("/v1/playerSessions/<arg>/member/%1").arg(group), Method::Delete,
                   [&db, &shared, spectator](const QString& sessionId,
                                             const QHttpServerRequest& req) -> QHttpServerResponse {
                       return HandleLeave(db, shared, sessionId, spectator, QString(), req);
                   });
        http.route(QStringLiteral("/v1/playerSessions/<arg>/member/%1/<arg>").arg(group),
                   Method::Delete,
                   [&db, &shared, spectator](const QString& sessionId, const QString& account,
                                             const QHttpServerRequest& req) -> QHttpServerResponse {
                       return HandleLeave(db, shared, sessionId, spectator, account, req);
                   });
    }

    http.route("/v1/playerSessions/<arg>/invitations", Method::Post,
               [&db, &shared](const QString& sessionId, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleInvite(db, shared, sessionId, req); });
    http.route("/v1/playerSessions/<arg>/sessionMessage", Method::Post,
               [&db, &shared](const QString& sessionId, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleMessage(db, shared, sessionId, req); });
    // A title that lost its session id posts to an empty path segment, which <arg> never
    // matches. Address the caller's session when it is unambiguous.
    http.route("/v1/playerSessions//sessionMessage", Method::Post,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   auto auth = WebApiAuth::Authenticate(req, db);
                   if (!auth.userId.has_value()) {
                       return std::move(auth.errorResponse);
                   }
                   const QStringList mine = SessionsOfUser(*auth.userId);
                   if (mine.size() != 1) {
                       qWarning() << "WebAPI: playerSession message with empty session id,"
                                  << "caller is in" << mine.size() << "session(s)";
                       return NotFound(QStringLiteral("sessionId"));
                   }
                   return HandleMessage(db, shared, mine.first(), req);
               });
    http.route("/v1/playerSessions/<arg>/leader", Method::Put,
               [&db, &shared](const QString& sessionId, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleLeader(db, shared, sessionId, req); });
    for (const QString field : {QStringLiteral("customData1"), QStringLiteral("customData2")}) {
        http.route(QStringLiteral("/v1/playerSessions/<arg>/%1").arg(field), Method::Put,
                   [&db, &shared, field](const QString& sessionId,
                                         const QHttpServerRequest& req) -> QHttpServerResponse {
                       return HandleCustomData(db, shared, sessionId, field, req);
                   });
    }

    http.route("/v1/friends/playerSessions", Method::Get,
               [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
                   return HandleFriendsSessions(db, shared, req);
               });
    http.route("/v1/users/<arg>/playerSessions", Method::Get,
               [&db, &shared](const QString& userKey, const QHttpServerRequest& req)
                   -> QHttpServerResponse { return HandleUserSessions(db, shared, userKey, req); });
}

void PurgeUserFromPlayerSessions(SharedState& shared, int64_t userId) {
    QList<Outgoing> events;
    {
        QMutexLocker lk(&g_mutex);
        QStringList closed;
        for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
            PsSession& s = it.value();
            s.invitedUserIds.remove(userId);
            if (!RemoveMember(s, userId, QStringLiteral("disconnected"), events)) {
                continue;
            }
            if (CountMembers(s, false) == 0) {
                closed << it.key();
            }
        }
        for (const QString& id : closed) {
            g_sessions.remove(id);
            qInfo() << "WebAPI: playerSession" << id << "closed (no players left)";
        }
    }
    Dispatch(shared, events);
}

} // namespace WebApiRoutes
