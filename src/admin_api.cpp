// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "admin_api.h"

#include <cmath>

#include <QByteArray>
#include <QDebug>
#include <QHostAddress>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QReadWriteLock>
#include <QStringList>
#include <QUrlQuery>

#include "client_session.h" // SharedState
#include "proto_utils.h"
#include "score_cache.h"
#include "score_files.h"
#include "shadnet.pb.h"
#include "trophy_config.h"
#include "version.h"
#include "worlds_service.h"

namespace {

// Error codes returned in the JSON body.
constexpr int ERR_BAD_REQUEST = 4000;
constexpr int ERR_INVALID_CREDENTIALS = 4010;
constexpr int ERR_MISSING_TOKEN = 4011;
constexpr int ERR_BAD_API_KEY = 4013;
constexpr int ERR_INVALID_TOKEN = 4012;
constexpr int ERR_NOT_ADMIN = 4030;
constexpr int ERR_FORBIDDEN_TARGET = 4031;
constexpr int ERR_NOT_FOUND = 4040;
constexpr int ERR_CONFLICT = 4090;
constexpr int ERR_TOO_MANY_ATTEMPTS = 4290;
constexpr int ERR_INTERNAL = 5000;

// Shortest password an admin may set. Not a policy engine — just a floor that stops
// a support reset from handing out something trivially guessable.
constexpr int kMinPasswordLength = 8;

QString GeneratePassword() {
    // No look-alike characters (0/O, 1/l/I): these get read aloud and typed by hand.
    static const QString chars = QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ"
                                                "abcdefghijkmnopqrstuvwxyz"
                                                "23456789");
    QString out;
    out.reserve(14);
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < 14; ++i)
        out.append(chars.at(gen->bounded(chars.size())));
    return out;
}

constexpr int kMaxLoginFailures = 5;
constexpr int kLoginBlockSeconds = 300;

QHttpServerResponse JsonError(QHttpServerResponse::StatusCode status, int code,
                              const QString& message) {
    QJsonObject err;
    err.insert(QStringLiteral("code"), code);
    err.insert(QStringLiteral("message"), message);
    QJsonObject body;
    body.insert(QStringLiteral("error"), err);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

QHttpServerResponse JsonOk(const QJsonObject& body) {
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::Ok};
}

std::optional<QJsonObject> ParseJsonBody(const QHttpServerRequest& req, QString& error) {
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        error = QStringLiteral("Body is not valid JSON: %1").arg(perr.errorString());
        return std::nullopt;
    }
    if (!doc.isObject()) {
        error = QStringLiteral("Body must be a JSON object");
        return std::nullopt;
    }
    return doc.object();
}

UserFilter ParseFilter(const QString& raw) {
    const QString f = raw.toLower();
    if (f == QLatin1String("banned"))
        return UserFilter::BannedOnly;
    if (f == QLatin1String("admins"))
        return UserFilter::AdminsOnly;
    if (f == QLatin1String("active"))
        return UserFilter::ActiveOnly;
    if (f == QLatin1String("online"))
        return UserFilter::OnlineOnly;
    if (f == QLatin1String("scores"))
        return UserFilter::WithScores;
    if (f == QLatin1String("trophies"))
        return UserFilter::WithTrophies;
    return UserFilter::All;
}

bool SecretsEqual(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

QString PeerKey(const QHttpServerRequest& req) {
    const QHostAddress addr = req.remoteAddress();
    return addr.isNull() ? QStringLiteral("unknown") : addr.toString();
}

const QString WebApiFriendDataType = QStringLiteral("np:service:friendlist:friend");

template <typename T>
QByteArray FriendNotifPayload(const T& msg) {
    QByteArray buf;
    appendProto(buf, msg);
    return buf;
}

QList<QPair<QString, QString>> FriendExtd(const QString& friendNpid, int64_t friendAccountId,
                                          const char* event) {
    const QString ev = QString::fromLatin1(event);
    QJsonObject trigger;
    trigger.insert(QStringLiteral("friend"), friendNpid);
    trigger.insert(QStringLiteral("event"), ev);
    QJsonObject addl;
    addl.insert(QStringLiteral("friendAccountId"), QString::number(friendAccountId));
    addl.insert(QStringLiteral("event"), ev);
    return {
        {QStringLiteral("trigger"),
         QString::fromUtf8(QJsonDocument(trigger).toJson(QJsonDocument::Compact))},
        {QStringLiteral("additionalTrigger"),
         QString::fromUtf8(QJsonDocument(addl).toJson(QJsonDocument::Compact))},
    };
}

void ApplyFriendChange(SharedState* shared, int64_t aId, const QString& aNpid, int64_t bId,
                       const QString& bNpid, bool wasMutual, bool isMutual) {
    if (!shared)
        return;
    struct Outgoing {
        std::function<void(QByteArray)> send;
        QByteArray packet;
    };
    QList<Outgoing> out;
    {
        QWriteLocker lk(&shared->clientsLock);
        auto aIt = shared->clients.find(aId);
        auto bIt = shared->clients.find(bId);
        const bool aOnline = aIt != shared->clients.end();
        const bool bOnline = bIt != shared->clients.end();

        if (isMutual) {
            if (aOnline)
                aIt->friends.insert(bId, bNpid);
            if (bOnline)
                bIt->friends.insert(aId, aNpid);
        } else {
            if (aOnline)
                aIt->friends.remove(bId);
            if (bOnline)
                bIt->friends.remove(aId);
        }

        const char* event = isMutual ? "add" : "remove";
        auto queueFor = [&](decltype(aIt) it, bool online, int64_t selfId, const QString& selfNpid,
                            int64_t otherId, const QString& otherNpid, bool otherOnline) {
            if (!online || !it->send)
                return;
            if (isMutual != wasMutual) {
                if (isMutual) {
                    shadnet::NotifyFriendNew n;
                    n.set_npid(otherNpid.toStdString());
                    n.set_online(otherOnline);
                    out.append({it->send, ClientSession::BuildNotification(
                                              NotificationType::FriendNew, FriendNotifPayload(n))});
                } else {
                    shadnet::NotifyFriendLost n;
                    n.set_npid(otherNpid.toStdString());
                    out.append(
                        {it->send, ClientSession::BuildNotification(NotificationType::FriendLost,
                                                                    FriendNotifPayload(n))});
                }
            }
            out.append(
                {it->send, ClientSession::BuildNotification(
                               NotificationType::WebApiPushEvent,
                               ClientSession::BuildWebApiPushPayload(
                                   QString(), 0, WebApiFriendDataType, QByteArray(), QString(),
                                   selfNpid, FriendExtd(otherNpid, otherId, event), 0, selfId))});
        };
        queueFor(aIt, aOnline, aId, aNpid, bId, bNpid, bOnline);
        queueFor(bIt, bOnline, bId, bNpid, aId, aNpid, aOnline);
    }
    for (const Outgoing& o : out)
        o.send(o.packet);
}

QJsonArray RelationshipArray(const QList<QPair<int64_t, QString>>& rows, SharedState* shared) {
    QJsonArray arr;
    std::optional<QReadLocker> lk;
    if (shared)
        lk.emplace(&shared->clientsLock);
    for (const auto& r : rows) {
        QJsonObject o;
        o.insert(QStringLiteral("userId"), static_cast<qint64>(r.first));
        o.insert(QStringLiteral("npid"), r.second);
        o.insert(QStringLiteral("online"), shared && shared->clients.contains(r.first));
        arr.append(o);
    }
    return arr;
}

} // namespace

AdminApiServer::AdminApiServer(QObject* parent) : QObject(parent) {}
AdminApiServer::~AdminApiServer() = default;

bool AdminApiServer::Start(ConfigManager* config, const QString& dbPath, SharedState* shared) {
    m_config = config;
    m_shared = shared;
    m_sessionMinutes = qMax(5, config->GetAdminSessionMinutes());

    m_db = std::make_unique<Database>(QStringLiteral("admin_api"));
    if (!m_db->Open(dbPath)) {
        qCritical() << "AdminApiServer: failed to open database at" << dbPath;
        return false;
    }

    const int promoted = SyncConfigAdmins();
    if (promoted > 0)
        qInfo() << "AdminApiServer: promoted" << promoted << "account(s) from AdminsList";

    if (m_db->CountUsersWhere(UserFilter::AdminsOnly) == 0) {
        qWarning() << "AdminApiServer: no account has admin rights yet. Add npids to "
                      "AdminsList in shadnet.cfg and restart to grant access.";
    }

    m_http = std::make_unique<QHttpServer>(this);
    RegisterRoutes();

    m_tcp = std::make_unique<QTcpServer>(this);
    const QString host = m_config->GetAdminApiHost();
    const quint16 port = m_config->GetAdminApiPort().toUShort();

    const bool loopback = (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1") ||
                           host == QStringLiteral("localhost"));

    if (!m_config->IsAdminApiKeyRequired() && !loopback) {
        const QString generated = m_config->EnsureAdminApiKey();
        if (generated.isEmpty()) {
            qCritical() << "AdminApiServer: refusing to start. AdminApiHost is" << host
                        << "but AdminApiKey is empty and a key could not be generated. Set "
                           "AdminApiKey in shadnet.cfg (for example: openssl rand -hex 32), "
                           "or bind AdminApiHost to 127.0.0.1.";
            return false;
        }
        qWarning().noquote()
            << "\n"
               "  ┌──────────────────────────────────────────────────────────────────────┐\n"
               "  │ A new admin API key was generated and saved to shadnet.cfg.          │\n"
               "  │ Use this key with admin API requests. It is not shown again.        │\n"
               "  └──────────────────────────────────────────────────────────────────────┘\n"
               "\n    AdminApiKey = "
            << generated << "\n";
    }

    if (m_config->IsAdminApiKeyRequired()) {
        qInfo() << "AdminApiServer: API key required on every request";
    } else {
        qWarning() << "AdminApiServer: no AdminApiKey set. Sign-in needs only an admin npid "
                      "and password. Acceptable on loopback; set a key before changing "
                      "AdminApiHost.";
    }
    if (!m_tcp->listen(QHostAddress(host), port)) {
        qCritical() << "AdminApiServer: failed to bind" << host << ":" << port << "—"
                    << m_tcp->errorString();
        return false;
    }
    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "AdminApiServer: QHttpServer failed to attach to listener";
        return false;
    }

    qInfo().nospace().noquote() << "AdminApiServer listening on: " << host << ":" << port;
    if (!loopback) {
        qWarning() << "AdminApiServer is reachable beyond localhost. Traffic is plain HTTP, so "
                      "the API key and admin password cross the network in the clear: put it "
                      "behind a TLS reverse proxy or a VPN.";
    }
    return true;
}

int AdminApiServer::SyncConfigAdmins() {
    int promoted = 0;
    const QStringList admins = m_config->GetAdminsList();
    for (const QString& npid : admins) {
        const QString trimmed = npid.trimmed();
        if (trimmed.isEmpty())
            continue;
        const auto uid = m_db->GetUserId(trimmed);
        if (!uid) {
            qWarning() << "AdminsList names an account that does not exist:" << trimmed;
            continue;
        }
        const auto row = m_db->GetUserRow(*uid);
        if (row && row->admin)
            continue; // already an admin, nothing to do
        if (m_db->SetAdmin(*uid, true)) {
            qInfo() << "Granted admin to" << trimmed;
            m_db->AddAuditEntry(0, QStringLiteral("config"), QStringLiteral("grant_admin"), *uid,
                                trimmed, QStringLiteral("Listed in AdminsList"));
            ++promoted;
        }
    }
    return promoted;
}

// Auth

QString AdminApiServer::MintToken(int64_t userId, const QString& npid) {
    static const QString chars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                "abcdefghijklmnopqrstuvwxyz"
                                                "0123456789");
    QString token;
    token.reserve(48);
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < 48; ++i)
        token.append(chars.at(gen->bounded(chars.size())));

    AdminSession s;
    s.userId = userId;
    s.npid = npid;
    s.expiresAt = QDateTime::currentDateTimeUtc().addSecs(m_sessionMinutes * 60);

    QMutexLocker lk(&m_sessionsMutex);
    m_sessions.insert(token, s);
    return token;
}

void AdminApiServer::PurgeExpiredSessions() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QMutexLocker lk(&m_sessionsMutex);
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->expiresAt <= now)
            it = m_sessions.erase(it);
        else
            ++it;
    }
}

std::optional<AdminApiServer::AdminSession> AdminApiServer::Authenticate(
    const QHttpServerRequest& req) {
    if (!CheckApiKey(req))
        return std::nullopt;

    const QByteArray rawAuth = req.value("Authorization");
    if (rawAuth.isEmpty())
        return std::nullopt;

    const QString authStr = QString::fromUtf8(rawAuth).trimmed();
    static const QString prefix = QStringLiteral("Bearer ");
    if (!authStr.startsWith(prefix, Qt::CaseInsensitive))
        return std::nullopt;
    const QString token = authStr.mid(prefix.size()).trimmed();
    if (token.isEmpty())
        return std::nullopt;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QMutexLocker lk(&m_sessionsMutex);
    auto it = m_sessions.find(token);
    if (it == m_sessions.end())
        return std::nullopt;
    if (it->expiresAt <= now) {
        m_sessions.erase(it);
        return std::nullopt;
    }
    // Sliding expiry: an admin actively using the tool stays logged in.
    it->expiresAt = now.addSecs(m_sessionMinutes * 60);
    return *it;
}

bool AdminApiServer::CheckApiKey(const QHttpServerRequest& req) const {
    const QString configured = m_config->GetAdminApiKey();
    if (configured.isEmpty())
        return true; // no key configured, nothing to check
    return SecretsEqual(req.value("X-Admin-Api-Key"), configured.toUtf8());
}

QHttpServerResponse AdminApiServer::ApiKeyError(const QHttpServerRequest& req) const {
    const bool absent = req.value("X-Admin-Api-Key").isEmpty();
    qWarning().nospace().noquote() << "AdminApi: rejected request from " << PeerKey(req)
                                   << " — API key " << (absent ? "not supplied" : "did not match");
    return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_BAD_API_KEY,
                     QStringLiteral("This server requires an admin API key in the X-Admin-Api-Key "
                                    "header. Ask a server operator for the value of AdminApiKey "
                                    "in shadnet.cfg."));
}

QHttpServerResponse AdminApiServer::AuthError(const QHttpServerRequest& req) const {
    if (!CheckApiKey(req))
        return ApiKeyError(req);

    const QString authStr = QString::fromUtf8(req.value("Authorization")).trimmed();
    if (authStr.isEmpty()) {
        return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_MISSING_TOKEN,
                         QStringLiteral("This endpoint needs an admin token. Sign in at "
                                        "/admin/v1/login and send it as an Authorization: "
                                        "Bearer header."));
    }
    return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_INVALID_TOKEN,
                     QStringLiteral("Your session is no longer valid. Sign in again."));
}

bool AdminApiServer::IsThrottled(const QString& peer, int& retryAfterSecs) {
    QMutexLocker lk(&m_sessionsMutex);
    auto it = m_throttle.find(peer);
    if (it == m_throttle.end() || it->blockedUntil.isNull())
        return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (it->blockedUntil <= now) {
        m_throttle.erase(it);
        return false;
    }
    retryAfterSecs = static_cast<int>(now.secsTo(it->blockedUntil));
    return true;
}

void AdminApiServer::NoteLoginFailure(const QString& peer) {
    QMutexLocker lk(&m_sessionsMutex);
    LoginThrottle& t = m_throttle[peer];
    if (++t.failures >= kMaxLoginFailures) {
        t.blockedUntil = QDateTime::currentDateTimeUtc().addSecs(kLoginBlockSeconds);
        t.failures = 0;
    }
}

void AdminApiServer::ClearLoginFailures(const QString& peer) {
    QMutexLocker lk(&m_sessionsMutex);
    m_throttle.remove(peer);
}

// Live session helpers

bool AdminApiServer::IsOnline(int64_t userId) const {
    if (!m_shared)
        return false;
    QReadLocker lk(&m_shared->clientsLock);
    return m_shared->clients.contains(userId);
}

int AdminApiServer::RevokeSessionsFor(int64_t userId) {
    QMutexLocker lk(&m_sessionsMutex);
    int dropped = 0;
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->userId == userId) {
            it = m_sessions.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

bool AdminApiServer::IsConfigManagedAdmin(const QString& npid) const {
    const QStringList admins = m_config->GetAdminsList();
    for (const QString& listed : admins) {
        if (listed.trimmed().compare(npid, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool AdminApiServer::KickUser(int64_t userId) {
    if (!m_shared)
        return false;
    std::function<void()> drop;
    {
        QReadLocker lk(&m_shared->clientsLock);
        const auto it = m_shared->clients.constFind(userId);
        if (it == m_shared->clients.constEnd() || !it->disconnect)
            return false;
        drop = it->disconnect;
    }
    // Called with the lock released: the session's own teardown takes the write lock.
    drop();
    return true;
}

bool AdminApiServer::DeleteAccount(int64_t userId, PurgeSummary& summary, int& cachedScoresDropped,
                                   int& blobsDeleted) {
    return DeleteAccountAndArtifacts(*m_db, m_shared, userId, summary, blobsDeleted,
                                     cachedScoresDropped);
}

QJsonObject AdminApiServer::UserRowToJson(const AdminUserRow& row) const {
    QJsonObject o;
    o.insert(QStringLiteral("userId"), static_cast<qint64>(row.userId));
    o.insert(QStringLiteral("npid"), row.username);
    o.insert(QStringLiteral("email"), row.email);
    o.insert(QStringLiteral("admin"), row.admin);
    o.insert(QStringLiteral("statAgent"), row.statAgent);
    o.insert(QStringLiteral("banned"), row.banned);
    o.insert(QStringLiteral("banReason"), row.banReason);
    o.insert(QStringLiteral("banTimestamp"), static_cast<qint64>(row.banTimestamp));
    o.insert(QStringLiteral("creation"), static_cast<qint64>(row.creation));
    o.insert(QStringLiteral("lastLogin"), static_cast<qint64>(row.lastLogin));
    o.insert(QStringLiteral("clientVersion"), row.clientVersion);
    o.insert(QStringLiteral("clientVersionAt"), static_cast<qint64>(row.clientVersionAt));
    o.insert(QStringLiteral("online"), IsOnline(row.userId));
    return o;
}

// ── Routes ────────────────────────────────────────────────────────────────────

void AdminApiServer::RegisterRoutes() {
    auto worldsRequest = [this](const QHttpServerRequest& req,
                                const QString& operation) -> QHttpServerResponse {
        const auto session = Authenticate(req);
        if (!session)
            return AuthError(req);
        const auto actor = m_db->GetUserRow(session->userId);
        if (!actor || !actor->admin || actor->banned)
            return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                             QStringLiteral("Current admin rights are required."));
        if (!m_shared || !m_shared->worlds)
            return JsonError(QHttpServerResponse::StatusCode::ServiceUnavailable, ERR_INTERNAL,
                             QStringLiteral("Worlds configuration is unavailable."));
        if (req.body().size() > kMaxWorldsBytes * 6 + 2048)
            return JsonError(QHttpServerResponse::StatusCode::PayloadTooLarge, ERR_BAD_REQUEST,
                             QStringLiteral("Worlds request is too large."));
        QJsonObject body;
        if (operation != QLatin1String("read")) {
            QString error;
            const auto parsed = ParseJsonBody(req, error);
            if (!parsed)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 error);
            body = *parsed;
        }
        const auto result = m_shared->worlds->Handle(operation, body);
        if (operation != QLatin1String("read")) {
            const QString action = QStringLiteral("worlds_") + operation +
                                   (result.ok ? QString() : QStringLiteral("_failed"));
            const QString detail =
                result.ok ? QStringLiteral("saved=%1 active=%2")
                                .arg(result.snapshot.revision, result.snapshot.activeRevision)
                          : result.message;
            if (!m_db->AddAuditEntry(session->userId, session->npid, action, 0,
                                     QStringLiteral("worlds.cfg"), detail))
                qWarning() << "Could not persist worlds audit:" << m_db->lastError();
            qInfo() << "Admin worlds" << operation << "by" << session->npid
                    << (result.ok ? "succeeded" : "failed") << detail;
        }
        if (!result.ok)
            return JsonError(static_cast<QHttpServerResponse::StatusCode>(result.status),
                             result.status * 10, result.message);
        return JsonOk(worldsSnapshotJson(result.snapshot));
    };
    m_http->route("/admin/v1/worlds/config", QHttpServerRequest::Method::Get,
                  [worldsRequest](const QHttpServerRequest& req) {
                      return worldsRequest(req, QStringLiteral("read"));
                  });
    m_http->route("/admin/v1/worlds/config", QHttpServerRequest::Method::Put,
                  [worldsRequest](const QHttpServerRequest& req) {
                      return worldsRequest(req, QStringLiteral("save"));
                  });
    m_http->route("/admin/v1/worlds/reload", QHttpServerRequest::Method::Post,
                  [worldsRequest](const QHttpServerRequest& req) {
                      return worldsRequest(req, QStringLiteral("reload"));
                  });

    m_http->route(
        "/admin/v1/status", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest&) {
            QJsonObject body;
            body.insert(QStringLiteral("ok"), true);
            body.insert(QStringLiteral("service"), QStringLiteral("shadnet-admin"));
            body.insert(QStringLiteral("apiVersion"), 1);
            body.insert(QStringLiteral("requiresApiKey"), m_config->IsAdminApiKeyRequired());
            body.insert(QStringLiteral("version"), ShadNet::Version());
            body.insert(QStringLiteral("buildTimestamp"), ShadNet::BuildTimestamp());
            return JsonOk(body);
        });

    // GET /admin/v1/version
    m_http->route("/admin/v1/version", QHttpServerRequest::Method::Get,
                  [](const QHttpServerRequest&) {
                      QJsonObject body;
                      body.insert(QStringLiteral("version"), ShadNet::Version());
                      body.insert(QStringLiteral("buildDate"), ShadNet::BuildDate());
                      body.insert(QStringLiteral("buildTime"), ShadNet::BuildTime());
                      body.insert(QStringLiteral("buildTimestamp"), ShadNet::BuildTimestamp());
                      return JsonOk(body);
                  });

    // POST /admin/v1/login — { npid, password } -> { token, expiresInSeconds, ... }
    m_http->route(
        "/admin/v1/login", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            const QString peer = PeerKey(req);
            int retryAfter = 0;
            if (IsThrottled(peer, retryAfter)) {
                return JsonError(
                    QHttpServerResponse::StatusCode::TooManyRequests, ERR_TOO_MANY_ATTEMPTS,
                    QStringLiteral("Too many failed sign-ins. Try again in %1 seconds.")
                        .arg(retryAfter));
            }
            if (!CheckApiKey(req)) {
                NoteLoginFailure(peer);
                return ApiKeyError(req);
            }

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);
            }
            const QString npid = bodyOpt->value(QStringLiteral("npid")).toString();
            const QString password = bodyOpt->value(QStringLiteral("password")).toString();
            if (npid.isEmpty() || password.isEmpty()) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Enter both an npid and a password."));
            }

            const auto user = m_db->CheckUser(npid, password, QString(), /*checkToken=*/false);
            if (!user) {
                NoteLoginFailure(peer);
                qWarning() << "AdminApi: failed sign-in for" << npid << "from" << peer;
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized,
                                 ERR_INVALID_CREDENTIALS,
                                 QStringLiteral("That npid and password don't match an account."));
            }
            if (user->banned) {
                NoteLoginFailure(peer);
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                                 QStringLiteral("This account is banned."));
            }
            if (!user->admin) {
                NoteLoginFailure(peer);
                qWarning() << "AdminApi: non-admin" << npid << "tried to sign in from" << peer;
                return JsonError(
                    QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                    QStringLiteral("This account doesn't have admin rights. Ask a server operator "
                                   "to add it to AdminsList in shadnet.cfg."));
            }

            ClearLoginFailures(peer);
            PurgeExpiredSessions();
            const QString token = MintToken(user->userId, user->username);
            qInfo() << "AdminApi: signed in" << user->username << "from" << peer;

            QJsonObject body;
            body.insert(QStringLiteral("token"), token);
            body.insert(QStringLiteral("expiresInSeconds"), m_sessionMinutes * 60);
            body.insert(QStringLiteral("userId"), static_cast<qint64>(user->userId));
            body.insert(QStringLiteral("npid"), user->username);
            return JsonOk(body);
        });

    // POST /admin/v1/logout — revokes the presented token.
    m_http->route("/admin/v1/logout", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      if (!CheckApiKey(req))
                          return ApiKeyError(req);

                      const QByteArray rawAuth = req.value("Authorization");
                      const QString authStr = QString::fromUtf8(rawAuth).trimmed();
                      if (authStr.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)) {
                          QMutexLocker lk(&m_sessionsMutex);
                          m_sessions.remove(authStr.mid(7).trimmed());
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("ok"), true);
                      return JsonOk(body);
                  });

    // GET /admin/v1/me — who the current token belongs to.
    m_http->route("/admin/v1/me", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);
                      QJsonObject body;
                      body.insert(QStringLiteral("userId"), static_cast<qint64>(session->userId));
                      body.insert(QStringLiteral("npid"), session->npid);
                      return JsonOk(body);
                  });

    // GET /admin/v1/overview — counts for the tool's status bar.
    m_http->route("/admin/v1/overview", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      int online = 0;
                      if (m_shared) {
                          QReadLocker lk(&m_shared->usageLock);
                          online = m_shared->usageTotalOnline;
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("totalUsers"), m_db->TotalUsers());
                      body.insert(QStringLiteral("bannedUsers"),
                                  m_db->CountUsersWhere(UserFilter::BannedOnly));
                      body.insert(QStringLiteral("adminUsers"),
                                  m_db->CountUsersWhere(UserFilter::AdminsOnly));
                      body.insert(QStringLiteral("onlineUsers"), online);
                      return JsonOk(body);
                  });

    // GET /admin/v1/users?search=&filter=&limit=&offset=
    m_http->route(
        "/admin/v1/users", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            const QUrlQuery query(req.url());
            const QString search = query.queryItemValue(QStringLiteral("search")).trimmed();
            const UserFilter filter = ParseFilter(query.queryItemValue(QStringLiteral("filter")));

            bool ok = false;
            int limit = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
            if (!ok)
                limit = 100;
            limit = qBound(1, limit, 500);
            int offset = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
            if (!ok || offset < 0)
                offset = 0;

            QJsonArray users;
            std::optional<QList<int64_t>> restrictToIds;
            if (filter == UserFilter::OnlineOnly) {
                QList<int64_t> online;
                if (m_shared) {
                    QReadLocker lk(&m_shared->clientsLock);
                    online.reserve(m_shared->clients.size());
                    for (auto it = m_shared->clients.constBegin();
                         it != m_shared->clients.constEnd(); ++it) {
                        online.append(it.key());
                    }
                }
                restrictToIds = online;
            }

            const auto rows = m_db->ListUsers(search, filter, limit, offset, restrictToIds);
            for (const AdminUserRow& row : rows)
                users.append(UserRowToJson(row));

            QJsonObject body;
            body.insert(QStringLiteral("users"), users);
            body.insert(QStringLiteral("total"), m_db->CountUsers(search, filter, restrictToIds));
            body.insert(QStringLiteral("limit"), limit);
            body.insert(QStringLiteral("offset"), offset);
            return JsonOk(body);
        });

    // POST /admin/v1/users — { npid, password?, email?, avatarUrl? }
    // Creates an account the same way in-game registration does.With no
    // password supplied the server generates one and returns it once
    m_http->route(
        "/admin/v1/users", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);
            const auto actor = m_db->GetUserRow(session->userId);
            if (!actor || !actor->admin || actor->banned)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                                 QStringLiteral("Current admin rights are required."));

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            const QString npid = bodyOpt->value(QStringLiteral("npid")).toString().trimmed();
            const QString supplied = bodyOpt->value(QStringLiteral("password")).toString();
            const QString email = bodyOpt->value(QStringLiteral("email")).toString().trimmed();
            QString avatarUrl = bodyOpt->value(QStringLiteral("avatarUrl")).toString().trimmed();

            if (!ClientSession::IsValidNpid(npid))
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("NP IDs must be 3–16 characters using letters, "
                                                "numbers, '-' or '_'."));
            if (!supplied.isEmpty() && supplied.length() < kMinPasswordLength)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Password must be at least %1 characters.")
                                     .arg(kMinPasswordLength));
            if (avatarUrl.isEmpty())
                avatarUrl =
                    QStringLiteral("https://shadps4.net/shad_net/assets/avatar/default_01.png");

            const bool generated = supplied.isEmpty();
            const QString password = generated ? GeneratePassword() : supplied;

            if (const auto err = m_db->CreateAccount(npid, password, avatarUrl, email)) {
                switch (*err) {
                case DbError::ExistingUsername:
                    return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                     QStringLiteral("The NP ID %1 is already taken.").arg(npid));
                case DbError::ExistingEmail:
                    return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                     QStringLiteral("Another account already uses %1.").arg(email));
                case DbError::InvalidEmail:
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     QStringLiteral("%1 isn't a valid email address.").arg(email));
                case DbError::InvalidInput:
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     QStringLiteral("The account details were rejected."));
                default:
                    qCritical() << "AdminApi: account creation failed for" << npid << ":"
                                << m_db->lastError();
                    return JsonError(QHttpServerResponse::StatusCode::InternalServerError,
                                     ERR_INTERNAL,
                                     QStringLiteral("The database rejected the account. "
                                                    "Check the server log."));
                }
            }

            const auto newId = m_db->GetUserId(npid);
            const auto row = newId ? m_db->GetUserRow(*newId) : std::nullopt;
            if (!row) {
                qCritical() << "AdminApi: created" << npid << "but could not read it back";
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The account was created but could not be read "
                                                "back. Refresh the list."));
            }

            // The password itself is never written to the log.
            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("create_account"),
                                row->userId, row->username,
                                generated ? QStringLiteral("password generated")
                                          : QStringLiteral("password set by admin"));
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " created account " << row->username;

            QJsonObject body;
            body.insert(QStringLiteral("ok"), true);
            body.insert(QStringLiteral("user"), UserRowToJson(*row));
            body.insert(QStringLiteral("generated"), generated);
            if (generated)
                body.insert(QStringLiteral("password"), password);
            if (m_config->IsEmailValidated()) {
                if (const auto rec = m_db->CheckUser(npid, password, QString(), false))
                    body.insert(QStringLiteral("token"), rec->token);
            }
            return JsonOk(body);
        });

    // GET /admin/v1/users/<id>
    m_http->route("/admin/v1/users/<arg>", QHttpServerRequest::Method::Get,
                  [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);
                      const auto row = m_db->GetUserRow(userId);
                      if (!row)
                          return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                           QStringLiteral("No account has id %1.").arg(userId));
                      return JsonOk(UserRowToJson(*row));
                  });

    // POST /admin/v1/users/<id>/ban — { banned: bool, reason?: string }
    m_http->route(
        "/admin/v1/users/<arg>/ban", QHttpServerRequest::Method::Post,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);
            if (!bodyOpt->contains(QStringLiteral("banned")))
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Body must set \"banned\" to true or false."));

            const bool banned = bodyOpt->value(QStringLiteral("banned")).toBool();
            const QString reason =
                bodyOpt->value(QStringLiteral("reason")).toString().trimmed().left(500);

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));

            // Two guards that keep the server administrable: an admin can't lock
            // themselves out, and admins can't ban each other. Demote first (via
            // AdminsList + restart, or directly in the DB) if you need to ban one.
            if (target->userId == session->userId)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                                 QStringLiteral("You can't ban your own account."));
            if (target->admin && banned)
                return JsonError(
                    QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                    QStringLiteral("%1 is an admin. Remove their admin rights before banning them.")
                        .arg(target->username));

            if (!m_db->BanUser(target->userId, banned, reason)) {
                qCritical() << "AdminApi: ban update failed for" << target->username << ":"
                            << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. "
                                                "Check the server log."));
            }

            // A ban only bites at the next login unless the live session is dropped.
            const bool kicked = banned ? KickUser(target->userId) : false;

            m_db->AddAuditEntry(session->userId, session->npid,
                                banned ? QStringLiteral("ban") : QStringLiteral("unban"),
                                target->userId, target->username, reason);

            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << (banned ? " banned " : " unbanned ")
                << target->username << (kicked ? " (session closed)" : "")
                << (reason.isEmpty() ? QString() : QStringLiteral(" — ") + reason);

            const auto updated = m_db->GetUserRow(target->userId);
            QJsonObject body;
            body.insert(QStringLiteral("user"),
                        updated ? UserRowToJson(*updated) : UserRowToJson(*target));
            body.insert(QStringLiteral("kicked"), kicked);
            return JsonOk(body);
        });

    // DELETE /admin/v1/users/<id> — removes the account and everything it produced.
    // Separate from ban on purpose: a ban is reversible and leaves the data alone,
    // this is neither.
    m_http->route(
        "/admin/v1/users/<arg>", QHttpServerRequest::Method::Delete,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            // A reason is optional here; an empty body is fine.
            QString reason;
            if (!req.body().trimmed().isEmpty()) {
                QString parseError;
                const auto bodyOpt = ParseJsonBody(req, parseError);
                if (!bodyOpt)
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     parseError);
                reason = bodyOpt->value(QStringLiteral("reason")).toString().trimmed().left(500);
            }

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));

            // The same two guards as banning, for the same reason — and they matter
            // more here, because this one can't be undone.
            if (target->userId == session->userId)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                                 QStringLiteral("You can't delete your own account."));
            if (target->admin)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                                 QStringLiteral("%1 is an admin. Remove their admin rights "
                                                "before deleting the account.")
                                     .arg(target->username));

            // Close the session first: once the account row is gone the user is
            // still holding an authenticated connection with no account behind it.
            const bool kicked = KickUser(target->userId);

            PurgeSummary summary;
            int cachedScoresDropped = 0;
            int blobsDeleted = 0;
            if (!DeleteAccount(target->userId, summary, cachedScoresDropped, blobsDeleted)) {
                qCritical() << "AdminApi: deleting" << target->username
                            << "failed:" << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the deletion, so nothing "
                                                "was removed. Check the server log."));
            }

            // The audit row outlives the account it refers to, which is the point:
            // it is the only remaining record that this npid ever existed.
            m_db->AddAuditEntry(
                session->userId, session->npid, QStringLiteral("delete_account"), target->userId,
                target->username,
                QStringLiteral("scores=%1 tusVariables=%2 tusData=%3 relationships=%4 "
                               "scoreBlobs=%5%6")
                    .arg(summary.scores)
                    .arg(summary.tusVariables)
                    .arg(summary.tusData)
                    .arg(summary.friendships)
                    .arg(blobsDeleted)
                    .arg(reason.isEmpty() ? QString() : QStringLiteral(" — ") + reason));

            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " deleted account " << target->username
                << (kicked ? " (session closed)" : "")
                << (reason.isEmpty() ? QString() : QStringLiteral(" — ") + reason);

            QJsonObject removed;
            removed.insert(QStringLiteral("scores"), summary.scores);
            removed.insert(QStringLiteral("trophies"), summary.trophies);
            removed.insert(QStringLiteral("scoreBlobs"), blobsDeleted);
            removed.insert(QStringLiteral("tusVariables"), summary.tusVariables);
            removed.insert(QStringLiteral("tusData"), summary.tusData);
            removed.insert(QStringLiteral("relationships"), summary.friendships);
            removed.insert(QStringLiteral("cachedScoresDropped"), cachedScoresDropped);
            removed.insert(QStringLiteral("total"), summary.total());

            QJsonObject body;
            body.insert(QStringLiteral("deleted"), true);
            body.insert(QStringLiteral("userId"), static_cast<qint64>(target->userId));
            body.insert(QStringLiteral("npid"), target->username);
            body.insert(QStringLiteral("kicked"), kicked);
            body.insert(QStringLiteral("removed"), removed);
            return JsonOk(body);
        });

    // POST /admin/v1/users/<id>/admin — { admin: bool, reason?: string }
    m_http->route(
        "/admin/v1/users/<arg>/admin", QHttpServerRequest::Method::Post,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);
            if (!bodyOpt->contains(QStringLiteral("admin")))
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Body must set \"admin\" to true or false."));

            const bool makeAdmin = bodyOpt->value(QStringLiteral("admin")).toBool();
            const QString reason =
                bodyOpt->value(QStringLiteral("reason")).toString().trimmed().left(500);

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));

            // Demoting yourself would end your own access mid-session, and there may
            // be no other admin to undo it. Another admin can still demote you.
            if (target->userId == session->userId)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                                 QStringLiteral("You can't change your own admin rights."));

            if (target->admin == makeAdmin) {
                QJsonObject body;
                body.insert(QStringLiteral("user"), UserRowToJson(*target));
                body.insert(QStringLiteral("changed"), false);
                return JsonOk(body);
            }

            // Granting admin to a banned account would hand moderation powers to
            // someone who cannot even sign in. Lift the ban first, deliberately.
            if (makeAdmin && target->banned)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN_TARGET,
                                 QStringLiteral("%1 is banned. Lift the ban before granting "
                                                "admin rights.")
                                     .arg(target->username));

            if (!m_db->SetAdmin(target->userId, makeAdmin)) {
                qCritical() << "AdminApi: SetAdmin failed for" << target->username << ":"
                            << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. "
                                                "Check the server log."));
            }

            // A revoked admin must lose their live admin session too, or their
            // bearer token keeps working until it expires on its own.
            const int sessionsDropped = makeAdmin ? 0 : RevokeSessionsFor(target->userId);

            m_db->AddAuditEntry(session->userId, session->npid,
                                makeAdmin ? QStringLiteral("grant_admin")
                                          : QStringLiteral("revoke_admin"),
                                target->userId, target->username, reason);

            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid
                << (makeAdmin ? " granted admin to " : " revoked admin from ") << target->username
                << (sessionsDropped > 0
                        ? QStringLiteral(" (%1 session(s) ended)").arg(sessionsDropped)
                        : QString());

            const auto updated = m_db->GetUserRow(target->userId);
            QJsonObject body;
            body.insert(QStringLiteral("user"),
                        updated ? UserRowToJson(*updated) : UserRowToJson(*target));
            body.insert(QStringLiteral("changed"), true);
            body.insert(QStringLiteral("sessionsEnded"), sessionsDropped);
            // AdminsList is applied at every startup, so revoking an account named
            // there only lasts until the next restart. Say so rather than letting
            // the change quietly reappear.
            body.insert(QStringLiteral("configManaged"), IsConfigManagedAdmin(target->username));
            return JsonOk(body);
        });

    // POST /admin/v1/users/<id>/kick — close a live game session without banning.
    m_http->route("/admin/v1/users/<arg>/kick", QHttpServerRequest::Method::Post,
                  [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      const auto target = m_db->GetUserRow(userId);
                      if (!target)
                          return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                           QStringLiteral("No account has id %1.").arg(userId));

                      const bool kicked = KickUser(target->userId);
                      if (kicked) {
                          m_db->AddAuditEntry(session->userId, session->npid,
                                              QStringLiteral("kick"), target->userId,
                                              target->username, QString());
                          qInfo().nospace().noquote() << "AdminApi: " << session->npid
                                                      << " disconnected " << target->username;
                      }

                      QJsonObject body;
                      body.insert(QStringLiteral("kicked"), kicked);
                      body.insert(QStringLiteral("npid"), target->username);
                      body.insert(QStringLiteral("userId"), static_cast<qint64>(target->userId));
                      return JsonOk(body);
                  });

    // POST /admin/v1/users/<id>/password — { password?: string }
    // With no password supplied the server generates one and returns it once.
    m_http->route(
        "/admin/v1/users/<arg>/password", QHttpServerRequest::Method::Post,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            QString supplied;
            if (!req.body().trimmed().isEmpty()) {
                QString parseError;
                const auto bodyOpt = ParseJsonBody(req, parseError);
                if (!bodyOpt)
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     parseError);
                supplied = bodyOpt->value(QStringLiteral("password")).toString();
            }

            if (!supplied.isEmpty() && supplied.length() < kMinPasswordLength)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Password must be at least %1 characters.")
                                     .arg(kMinPasswordLength));

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));

            const bool generated = supplied.isEmpty();
            const QString password = generated ? GeneratePassword() : supplied;

            if (!m_db->SetPassword(target->userId, password)) {
                qCritical() << "AdminApi: password reset failed for" << target->username << ":"
                            << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. "
                                                "Check the server log."));
            }

            // The account token was rotated, so the live game session is now holding
            // a credential the server no longer accepts. Close it rather than
            // leaving them connected in an undefined state.
            const bool kicked = KickUser(target->userId);

            // The password itself is never written to the log — only that it changed.
            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("reset_password"),
                                target->userId, target->username,
                                generated ? QStringLiteral("generated")
                                          : QStringLiteral("set by admin"));
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " reset the password for " << target->username;

            QJsonObject body;
            body.insert(QStringLiteral("ok"), true);
            body.insert(QStringLiteral("npid"), target->username);
            body.insert(QStringLiteral("kicked"), kicked);
            body.insert(QStringLiteral("generated"), generated);
            // Returned once, and only when the server made it up. A password the
            // admin chose is not echoed back.
            if (generated)
                body.insert(QStringLiteral("password"), password);
            return JsonOk(body);
        });

    // GET /admin/v1/online — who is connected right now, with their title context.
    m_http->route("/admin/v1/online", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      QJsonArray players;
                      if (m_shared) {
                          QReadLocker lk(&m_shared->clientsLock);
                          for (auto it = m_shared->clients.constBegin();
                               it != m_shared->clients.constEnd(); ++it) {
                              QJsonObject o;
                              o.insert(QStringLiteral("userId"), static_cast<qint64>(it.key()));
                              o.insert(QStringLiteral("npid"), it->npid);
                              o.insert(QStringLiteral("titleId"), it->npTitleId);
                              o.insert(QStringLiteral("titleName"), it->titleName);
                              o.insert(QStringLiteral("gameStatus"), it->gameStatus);
                              o.insert(QStringLiteral("platform"), it->platform);
                              o.insert(QStringLiteral("presenceUpdatedAt"), it->presenceUpdatedAt);
                              // Reported so an admin sees the true picture: appearing offline
                              // hides someone from other players, not from moderation.
                              o.insert(QStringLiteral("appearOffline"), it->appearOffline);
                              players.append(o);
                          }
                      }

                      QJsonObject body;
                      body.insert(QStringLiteral("players"), players);
                      body.insert(QStringLiteral("total"), players.size());
                      return JsonOk(body);
                  });

    // GET /admin/v1/boards — every board holding scores, most populated first.
    m_http->route("/admin/v1/boards", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      QJsonArray boards;
                      for (const auto& b : m_db->ListScoreBoards()) {
                          QJsonObject o;
                          o.insert(QStringLiteral("comId"), b.comId);
                          o.insert(QStringLiteral("titleName"), b.titleName);
                          o.insert(QStringLiteral("boardId"), static_cast<qint64>(b.boardId));
                          o.insert(QStringLiteral("scoreCount"), b.scoreCount);
                          boards.append(o);
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("boards"), boards);
                      return JsonOk(body);
                  });

    // GET /admin/v1/boards/<comId>/<boardId>/scores?limit=&offset=
    m_http->route("/admin/v1/boards/<arg>/<arg>/scores", QHttpServerRequest::Method::Get,
                  [this](const QString& comId, qint64 boardId,
                         const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      const QUrlQuery query(req.url());
                      bool ok = false;
                      int limit = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
                      if (!ok)
                          limit = 100;
                      limit = qBound(1, limit, 500);
                      int offset = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
                      if (!ok || offset < 0)
                          offset = 0;

                      const auto board = static_cast<uint32_t>(boardId);
                      QJsonArray scores;
                      int rank = offset;
                      for (const auto& r : m_db->ListBoardScores(comId, board, limit, offset)) {
                          QJsonObject o;
                          o.insert(QStringLiteral("rank"), ++rank);
                          o.insert(QStringLiteral("userId"), static_cast<qint64>(r.userId));
                          // Empty when the account is gone but the score somehow remains.
                          o.insert(QStringLiteral("npid"), r.npid);
                          o.insert(QStringLiteral("characterId"), r.characterId);
                          o.insert(QStringLiteral("score"), static_cast<qint64>(r.score));
                          o.insert(QStringLiteral("comment"), r.comment);
                          o.insert(QStringLiteral("hasGameData"), r.dataId != 0);
                          o.insert(QStringLiteral("timestamp"), static_cast<qint64>(r.timestamp));
                          scores.append(o);
                      }

                      QJsonObject body;
                      body.insert(QStringLiteral("comId"), comId);
                      body.insert(QStringLiteral("boardId"), boardId);
                      body.insert(QStringLiteral("scores"), scores);
                      body.insert(QStringLiteral("total"), m_db->CountBoardScores(comId, board));
                      body.insert(QStringLiteral("limit"), limit);
                      body.insert(QStringLiteral("offset"), offset);
                      return JsonOk(body);
                  });

    // DELETE /admin/v1/boards/<comId>/<boardId>/scores/<userId>?characterId=N
    // Removes a single posted score. Unlike deleting the account, this leaves the
    // player and everything else they own untouched.
    m_http->route(
        "/admin/v1/boards/<arg>/<arg>/scores/<arg>", QHttpServerRequest::Method::Delete,
        [this](const QString& comId, qint64 boardId, qint64 userId,
               const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            const QUrlQuery query(req.url());
            bool ok = false;
            const int characterId = query.queryItemValue(QStringLiteral("characterId")).toInt(&ok);
            const auto board = static_cast<uint32_t>(boardId);
            const auto charId = static_cast<int32_t>(ok ? characterId : 0);

            uint64_t dataId = 0;
            if (!m_db->DeleteScore(comId, board, userId, charId, dataId)) {
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No score on %1 board %2 for account %3 "
                                                "(character %4).")
                                     .arg(comId)
                                     .arg(boardId)
                                     .arg(userId)
                                     .arg(charId));
            }

            // The saved game data attached to the score is only reachable through
            // that row, so it would otherwise be orphaned on disk.
            bool blobDeleted = false;
            if (dataId != 0 && m_shared && m_shared->scoreFiles) {
                m_shared->scoreFiles->Remove(dataId);
                blobDeleted = true;
            }

            // Drop it from the live board too, or it keeps being served.
            bool cacheUpdated = false;
            if (m_shared && m_shared->scoreCache)
                cacheUpdated = m_shared->scoreCache->RemoveEntry(comId, board, userId, charId);

            const QString npid = m_db->GetUsername(userId).value_or(QString::number(userId));
            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("delete_score"),
                                userId, npid,
                                QStringLiteral("comId=%1 board=%2 character=%3")
                                    .arg(comId)
                                    .arg(boardId)
                                    .arg(charId));
            qInfo().nospace().noquote() << "AdminApi: " << session->npid << " deleted a score on "
                                        << comId << " board " << boardId << " for " << npid;

            QJsonObject body;
            body.insert(QStringLiteral("deleted"), true);
            body.insert(QStringLiteral("comId"), comId);
            body.insert(QStringLiteral("boardId"), boardId);
            body.insert(QStringLiteral("userId"), userId);
            body.insert(QStringLiteral("characterId"), charId);
            body.insert(QStringLiteral("gameDataDeleted"), blobDeleted);
            body.insert(QStringLiteral("removedFromLiveBoard"), cacheUpdated);
            return JsonOk(body);
        });

    // GET /admin/v1/titles — every communication id the server knows, named or not.
    m_http->route("/admin/v1/titles", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      QJsonArray titles;
                      int unnamed = 0;
                      for (const auto& t : m_db->ListKnownTitles()) {
                          QJsonObject o;
                          o.insert(QStringLiteral("comId"), t.comId);
                          o.insert(QStringLiteral("titleName"), t.titleName);
                          o.insert(QStringLiteral("hasScores"), t.hasScores);
                          o.insert(QStringLiteral("hasTrophies"), t.hasTrophies);
                          o.insert(QStringLiteral("hasTrophyNames"), t.hasTrophyNames);
                          titles.append(o);
                          if (t.titleName.isEmpty())
                              ++unnamed;
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("titles"), titles);
                      body.insert(QStringLiteral("total"), titles.size());
                      body.insert(QStringLiteral("unnamed"), unnamed);
                      return JsonOk(body);
                  });

    // PUT /admin/v1/titles/<comId> — { name } names or renames a game.
    m_http->route(
        "/admin/v1/titles/<arg>", QHttpServerRequest::Method::Put,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            if (comId.isEmpty() || comId.size() > 16)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Communication id must be 1-16 characters."));

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            QString name = bodyOpt->value(QStringLiteral("name")).toString().simplified();
            name.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
            name = name.left(128);

            if (name.isEmpty())
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Give the game a name, or send DELETE to "
                                                "remove the one it has."));

            const auto previous = m_db->GetTitleName(comId);
            if (!m_db->RenameTitle(comId, name)) {
                qCritical() << "AdminApi: rename failed for" << comId << ":" << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. "
                                                "Check the server log."));
            }

            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("rename_title"), 0,
                                comId,
                                previous && !previous->isEmpty()
                                    ? QStringLiteral("\"%1\" -> \"%2\"").arg(*previous, name)
                                    : QStringLiteral("named \"%1\"").arg(name));
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " named " << comId << " \"" << name << "\"";

            QJsonObject body;
            body.insert(QStringLiteral("comId"), comId);
            body.insert(QStringLiteral("titleName"), name);
            body.insert(QStringLiteral("previousName"), previous.value_or(QString()));
            return JsonOk(body);
        });

    // DELETE /admin/v1/titles/<comId> — forget a name; the id shows through again.
    m_http->route(
        "/admin/v1/titles/<arg>", QHttpServerRequest::Method::Delete,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            if (!m_db->ClearTitleName(comId))
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("%1 has no name stored.").arg(comId));

            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("clear_title"), 0,
                                comId, QString());
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " cleared the name for " << comId;

            QJsonObject body;
            body.insert(QStringLiteral("comId"), comId);
            body.insert(QStringLiteral("cleared"), true);
            return JsonOk(body);
        });

    // GET /admin/v1/trophies — games with trophy activity, most played first.
    m_http->route("/admin/v1/trophies", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      QJsonArray games;
                      for (const auto& g : m_db->ListTrophyGames()) {
                          QJsonObject o;
                          o.insert(QStringLiteral("comId"), g.comId);
                          o.insert(QStringLiteral("titleName"), g.titleName);
                          o.insert(QStringLiteral("players"), g.players);
                          o.insert(QStringLiteral("trophies"), g.trophies);
                          o.insert(QStringLiteral("unlocks"), g.unlocks);
                          o.insert(QStringLiteral("hasTrophyNames"),
                                   m_db->CountTrophyMeta(g.comId) > 0);
                          games.append(o);
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("games"), games);
                      return JsonOk(body);
                  });

    // GET /admin/v1/trophies/<comId> — earner counts and share, per trophy.
    m_http->route(
        "/admin/v1/trophies/<arg>", QHttpServerRequest::Method::Get,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            const int players = m_db->CountTrophyPlayers(comId);
            QJsonArray trophies;
            for (const auto& e : m_db->ListTrophyEarners(comId)) {
                QJsonObject o;
                o.insert(QStringLiteral("trophyId"), e.trophyId);
                o.insert(QStringLiteral("earners"), e.earners);
                o.insert(QStringLiteral("earnedPercent"),
                         players > 0 ? std::round(e.earners * 10000.0 / players) / 100.0 : 0.0);
                trophies.append(o);
            }
            QJsonObject body;
            body.insert(QStringLiteral("comId"), comId);
            body.insert(QStringLiteral("players"), players);
            body.insert(QStringLiteral("trophies"), trophies);
            return JsonOk(body);
        });

    // POST /admin/v1/trophies/<comId>/config — import a title's TROP.XML.
    m_http->route(
        "/admin/v1/trophies/<arg>/config", QHttpServerRequest::Method::Post,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            if (comId.isEmpty() || comId.size() > 12)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Communication id must be 1-12 characters."));

            const QByteArray body = req.body();
            if (body.isEmpty())
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Send the TROP.XML contents as the request body."));
            // A trophy config is tens of KB; far larger is not one.
            if (body.size() > 4 * 1024 * 1024)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("That file is too large to be a trophy config."));

            const QUrlQuery query(req.url());
            const QString language =
                query.queryItemValue(QStringLiteral("language")).trimmed().left(8);

            const TrophyConfig cfg = ParseTrophyConfig(body, language);
            if (!cfg.ok())
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 cfg.error);
            if (!cfg.comId.isEmpty() && cfg.comId.compare(comId, Qt::CaseInsensitive) != 0) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                    QStringLiteral("This file is for %1, not %2.").arg(cfg.comId, comId));
            }

            if (!m_db->ImportTrophyMeta(comId, cfg.trophies, cfg.groups)) {
                qCritical() << "AdminApi: trophy config import failed for" << comId << ":"
                            << m_db->lastError();
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the import, so nothing was "
                                                "changed. Check the server log."));
            }
            bool titleNamed = false;
            if (!cfg.titleName.isEmpty())
                titleNamed = m_db->SetTitleName(comId, cfg.titleName);

            m_db->AddAuditEntry(session->userId, session->npid,
                                QStringLiteral("import_trophy_config"), 0, comId,
                                QStringLiteral("trophies=%1 language=%2")
                                    .arg(cfg.trophies.size())
                                    .arg(language.isEmpty() ? QStringLiteral("master") : language));
            qInfo().nospace().noquote() << "AdminApi: " << session->npid << " imported "
                                        << cfg.trophies.size() << " trophy names for " << comId;

            QJsonObject out;
            out.insert(QStringLiteral("imported"), true);
            out.insert(QStringLiteral("comId"), comId);
            out.insert(QStringLiteral("trophies"), cfg.trophies.size());
            out.insert(QStringLiteral("groups"), cfg.groups.size());
            out.insert(QStringLiteral("titleName"), cfg.titleName);
            out.insert(QStringLiteral("titleNameApplied"), titleNamed);
            return JsonOk(out);
        });

    // GET /admin/v1/trophies/<comId>/config — what names are on record.
    m_http->route(
        "/admin/v1/trophies/<arg>/config", QHttpServerRequest::Method::Get,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            QJsonArray trophies;
            for (const auto& m : m_db->ListTrophyMeta(comId)) {
                QJsonObject o;
                o.insert(QStringLiteral("trophyId"), m.trophyId);
                o.insert(QStringLiteral("name"), m.name);
                o.insert(QStringLiteral("detail"), m.detail);
                o.insert(QStringLiteral("grade"), m.grade);
                o.insert(QStringLiteral("hidden"), m.hidden);
                o.insert(QStringLiteral("groupId"), m.groupId);
                o.insert(QStringLiteral("language"), m.language);
                trophies.append(o);
            }
            QJsonObject body;
            body.insert(QStringLiteral("comId"), comId);
            body.insert(QStringLiteral("trophies"), trophies);
            body.insert(QStringLiteral("total"), trophies.size());
            return JsonOk(body);
        });

    // DELETE /admin/v1/trophies/<comId>/config — forget a title's names. Earned
    // trophies are untouched; only the labels go.
    m_http->route(
        "/admin/v1/trophies/<arg>/config", QHttpServerRequest::Method::Delete,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);

            if (!m_db->DeleteTrophyMeta(comId))
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No trophy names are stored for %1.").arg(comId));

            m_db->AddAuditEntry(session->userId, session->npid,
                                QStringLiteral("clear_trophy_config"), 0, comId, QString());
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " cleared trophy names for " << comId;

            QJsonObject body;
            body.insert(QStringLiteral("deleted"), true);
            body.insert(QStringLiteral("comId"), comId);
            return JsonOk(body);
        });

    // GET /admin/v1/users/<id>/friends — friends, pending requests both ways, and blocks.
    m_http->route(
        "/admin/v1/users/<arg>/friends", QHttpServerRequest::Method::Get,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);
            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));

            const UserRelationships rel = m_db->GetRelationships(userId);
            QJsonObject body;
            body.insert(QStringLiteral("userId"), static_cast<qint64>(target->userId));
            body.insert(QStringLiteral("npid"), target->username);
            body.insert(QStringLiteral("friends"), RelationshipArray(rel.friends, m_shared));
            body.insert(QStringLiteral("requestsSent"),
                        RelationshipArray(rel.friendRequestsSent, m_shared));
            body.insert(QStringLiteral("requestsReceived"),
                        RelationshipArray(rel.friendRequestsReceived, m_shared));
            body.insert(QStringLiteral("blocked"), RelationshipArray(rel.blocked, m_shared));
            return JsonOk(body);
        });

    // POST /admin/v1/users/<id>/friends — { npid } makes the two accounts friends on both
    // sides at once (no request/accept step). A pending request either way is completed.
    // Refused when either side has blocked the other: blocks are the player's choice.
    m_http->route(
        "/admin/v1/users/<arg>/friends", QHttpServerRequest::Method::Post,
        [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);
            const auto actor = m_db->GetUserRow(session->userId);
            if (!actor || !actor->admin || actor->banned)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                                 QStringLiteral("Current admin rights are required."));

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);
            const QString friendNpidIn =
                bodyOpt->value(QStringLiteral("npid")).toString().trimmed();
            if (friendNpidIn.isEmpty())
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Body must name the friend's \"npid\"."));

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));
            const auto friendIdOpt = m_db->GetUserId(friendNpidIn);
            const auto other = friendIdOpt ? m_db->GetUserRow(*friendIdOpt) : std::nullopt;
            if (!other)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account is called %1.").arg(friendNpidIn));
            if (other->userId == target->userId)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("An account can't be its own friend."));

            constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
            constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);
            const auto [res, rel] = m_db->GetRelStatus(target->userId, other->userId);
            if (res == Database::RelResult::Error)
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("Couldn't read the relationship. Check the "
                                                "server log."));
            const uint8_t cur1 = res == Database::RelResult::Ok ? rel.caller : 0;
            const uint8_t cur2 = res == Database::RelResult::Ok ? rel.other : 0;
            if (cur1 & B)
                return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                 QStringLiteral("%1 has blocked %2. They need to unblock first.")
                                     .arg(target->username, other->username));
            if (cur2 & B)
                return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                 QStringLiteral("%1 has blocked %2. They need to unblock first.")
                                     .arg(other->username, target->username));
            if ((cur1 & F) && (cur2 & F))
                return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                 QStringLiteral("%1 and %2 are already friends.")
                                     .arg(target->username, other->username));

            if (!m_db->SetRelStatus(target->userId, other->userId, cur1 | F, cur2 | F))
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. Check the "
                                                "server log."));

            ApplyFriendChange(m_shared, target->userId, target->username, other->userId,
                              other->username, /*wasMutual=*/false, /*isMutual=*/true);

            m_db->AddAuditEntry(session->userId, session->npid, QStringLiteral("add_friend"),
                                target->userId, target->username,
                                QStringLiteral("with %1").arg(other->username));
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " made " << target->username << " and "
                << other->username << " friends";

            QJsonObject body;
            body.insert(QStringLiteral("ok"), true);
            body.insert(QStringLiteral("userId"), static_cast<qint64>(target->userId));
            body.insert(QStringLiteral("npid"), target->username);
            body.insert(QStringLiteral("friendUserId"), static_cast<qint64>(other->userId));
            body.insert(QStringLiteral("friendNpid"), other->username);
            return JsonOk(body);
        });

    // DELETE /admin/v1/users/<id>/friends/<friendId> — ends the friendship on both sides,
    // or cancels a pending request in either direction. Blocks are left as they are.
    m_http->route(
        "/admin/v1/users/<arg>/friends/<arg>", QHttpServerRequest::Method::Delete,
        [this](qint64 userId, qint64 friendId,
               const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto session = Authenticate(req);
            if (!session)
                return AuthError(req);
            const auto actor = m_db->GetUserRow(session->userId);
            if (!actor || !actor->admin || actor->banned)
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_NOT_ADMIN,
                                 QStringLiteral("Current admin rights are required."));

            const auto target = m_db->GetUserRow(userId);
            if (!target)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(userId));
            const auto other = m_db->GetUserRow(friendId);
            if (!other)
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("No account has id %1.").arg(friendId));

            constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
            const auto [res, rel] = m_db->GetRelStatus(target->userId, other->userId);
            if (res == Database::RelResult::Error)
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("Couldn't read the relationship. Check the "
                                                "server log."));
            if (res != Database::RelResult::Ok || (!(rel.caller & F) && !(rel.other & F)))
                return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                 QStringLiteral("%1 and %2 aren't friends and have no pending "
                                                "request.")
                                     .arg(target->username, other->username));

            const bool wasMutual = (rel.caller & F) && (rel.other & F);
            const uint8_t new1 = rel.caller & static_cast<uint8_t>(~F);
            const uint8_t new2 = rel.other & static_cast<uint8_t>(~F);
            const bool ok = (new1 == 0 && new2 == 0)
                                ? m_db->DeleteRel(target->userId, other->userId)
                                : m_db->SetRelStatus(target->userId, other->userId, new1, new2);
            if (!ok)
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The database rejected the change. Check the "
                                                "server log."));

            ApplyFriendChange(m_shared, target->userId, target->username, other->userId,
                              other->username, wasMutual, /*isMutual=*/false);

            m_db->AddAuditEntry(
                session->userId, session->npid, QStringLiteral("remove_friend"), target->userId,
                target->username,
                (wasMutual ? QStringLiteral("with %1") : QStringLiteral("pending request with %1"))
                    .arg(other->username));
            qInfo().nospace().noquote()
                << "AdminApi: " << session->npid << " removed "
                << (wasMutual ? "the friendship" : "the friend request") << " between "
                << target->username << " and " << other->username;

            QJsonObject body;
            body.insert(QStringLiteral("ok"), true);
            body.insert(QStringLiteral("userId"), static_cast<qint64>(target->userId));
            body.insert(QStringLiteral("npid"), target->username);
            body.insert(QStringLiteral("friendUserId"), static_cast<qint64>(other->userId));
            body.insert(QStringLiteral("friendNpid"), other->username);
            body.insert(QStringLiteral("wasFriend"), wasMutual);
            return JsonOk(body);
        });

    // GET /admin/v1/users/<id>/trophies — everything one account has unlocked.
    m_http->route("/admin/v1/users/<arg>/trophies", QHttpServerRequest::Method::Get,
                  [this](qint64 userId, const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      const auto target = m_db->GetUserRow(userId);
                      if (!target)
                          return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                           QStringLiteral("No account has id %1.").arg(userId));

                      QJsonArray trophies;
                      for (const auto& t : m_db->ListUserTrophies(userId)) {
                          QJsonObject o;
                          o.insert(QStringLiteral("comId"), t.comId);
                          o.insert(QStringLiteral("titleName"), t.titleName);
                          o.insert(QStringLiteral("trophyId"), t.trophyId);
                          o.insert(QStringLiteral("earnedAt"), static_cast<qint64>(t.earnedAt));
                          trophies.append(o);
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("userId"), userId);
                      body.insert(QStringLiteral("npid"), target->username);
                      body.insert(QStringLiteral("trophies"), trophies);
                      body.insert(QStringLiteral("total"), trophies.size());
                      return JsonOk(body);
                  });

    // DELETE /admin/v1/users/<id>/trophies/<comId>/<trophyId>
    // Revokes one unlock, for a trophy obtained by tampering. The account and
    // everything else it holds are untouched.
    m_http->route("/admin/v1/users/<arg>/trophies/<arg>/<arg>", QHttpServerRequest::Method::Delete,
                  [this](qint64 userId, const QString& comId, qint64 trophyId,
                         const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      const auto target = m_db->GetUserRow(userId);
                      if (!target)
                          return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                           QStringLiteral("No account has id %1.").arg(userId));

                      if (!m_db->DeleteUserTrophy(userId, comId, static_cast<int32_t>(trophyId))) {
                          return JsonError(QHttpServerResponse::StatusCode::NotFound, ERR_NOT_FOUND,
                                           QStringLiteral("%1 has no trophy %2 in %3.")
                                               .arg(target->username)
                                               .arg(trophyId)
                                               .arg(comId));
                      }

                      m_db->AddAuditEntry(
                          session->userId, session->npid, QStringLiteral("revoke_trophy"), userId,
                          target->username,
                          QStringLiteral("comId=%1 trophy=%2").arg(comId).arg(trophyId));
                      qInfo().nospace().noquote()
                          << "AdminApi: " << session->npid << " revoked trophy " << trophyId
                          << " in " << comId << " from " << target->username;

                      QJsonObject body;
                      body.insert(QStringLiteral("deleted"), true);
                      body.insert(QStringLiteral("npid"), target->username);
                      body.insert(QStringLiteral("comId"), comId);
                      body.insert(QStringLiteral("trophyId"), trophyId);
                      return JsonOk(body);
                  });

    // GET /admin/v1/audit?limit=&offset= — who did what, most recent first.
    m_http->route("/admin/v1/audit", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      const auto session = Authenticate(req);
                      if (!session)
                          return AuthError(req);

                      const QUrlQuery query(req.url());
                      bool ok = false;
                      int limit = query.queryItemValue(QStringLiteral("limit")).toInt(&ok);
                      if (!ok)
                          limit = 100;
                      limit = qBound(1, limit, 500);
                      int offset = query.queryItemValue(QStringLiteral("offset")).toInt(&ok);
                      if (!ok || offset < 0)
                          offset = 0;

                      QJsonArray entries;
                      for (const AuditRow& row : m_db->ListAudit(limit, offset)) {
                          QJsonObject o;
                          o.insert(QStringLiteral("id"), static_cast<qint64>(row.id));
                          o.insert(QStringLiteral("timestamp"), static_cast<qint64>(row.timestamp));
                          o.insert(QStringLiteral("actorNpid"), row.actorNpid);
                          o.insert(QStringLiteral("action"), row.action);
                          o.insert(QStringLiteral("targetNpid"), row.targetNpid);
                          o.insert(QStringLiteral("targetUserId"),
                                   static_cast<qint64>(row.targetUserId));
                          o.insert(QStringLiteral("reason"), row.reason);
                          entries.append(o);
                      }
                      QJsonObject body;
                      body.insert(QStringLiteral("entries"), entries);
                      return JsonOk(body);
                  });

    m_http->setMissingHandler(
        this, [](const QHttpServerRequest& req, QHttpServerResponder& responder) {
            qWarning() << "AdminApi: unhandled" << req.method() << req.url().path();
            QJsonObject err;
            err.insert(QStringLiteral("code"), ERR_NOT_FOUND);
            err.insert(QStringLiteral("message"), QStringLiteral("No such admin endpoint."));
            QJsonObject body;
            body.insert(QStringLiteral("error"), err);
            responder.sendResponse(QHttpServerResponse{
                "application/json", QJsonDocument(body).toJson(QJsonDocument::Compact),
                QHttpServerResponder::StatusCode::NotFound});
        });
}
