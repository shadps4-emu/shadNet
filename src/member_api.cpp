// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "member_api.h"

#include <cmath>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

constexpr int ERR_BAD_REQUEST = 400;
constexpr int ERR_UNAUTHORIZED = 401;
constexpr int ERR_FORBIDDEN = 403;
constexpr int ERR_CONFLICT = 409;
constexpr int ERR_TOO_MANY = 429;
constexpr int ERR_INTERNAL = 500;
constexpr int ERR_BAD_API_KEY = 498;

constexpr int MaxFailuresPerPeer = 8;
constexpr int FailureWindowSeconds = 300;
constexpr int BlockSeconds = 900;

constexpr int SessionMinutes = 24 * 60;

// PlayStation NP IDs are at most 16 characters.
constexpr int MaxNpidLength = 16;
constexpr int MinPasswordLength = 8;
constexpr int MaxPasswordLength = 200;

QHttpServerResponse JsonError(QHttpServerResponse::StatusCode status, int code,
                              const QString& message) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject body;
    body.insert(QStringLiteral("error"), error);
    return QHttpServerResponse(QJsonDocument(body).object(), status);
}

QHttpServerResponse JsonOk(const QJsonObject& body) {
    return QHttpServerResponse(body, QHttpServerResponse::StatusCode::Ok);
}

std::optional<QJsonObject> ParseJsonBody(const QHttpServerRequest& req, QString& error) {
    const QByteArray raw = req.body();
    if (raw.isEmpty()) {
        error = QStringLiteral("The request had no body.");
        return std::nullopt;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("The request body is not valid JSON.");
        return std::nullopt;
    }
    return doc.object();
}

bool SecretsEqual(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

QString BearerToken(const QHttpServerRequest& req) {
    const QByteArray rawAuth = req.value("Authorization");
    if (rawAuth.isEmpty())
        return {};
    const QString authStr = QString::fromUtf8(rawAuth).trimmed();
    static const QString prefix = QStringLiteral("Bearer ");
    if (!authStr.startsWith(prefix, Qt::CaseInsensitive))
        return {};
    return authStr.mid(prefix.size()).trimmed();
}

QString PeerOf(const QHttpServerRequest& req) {
    return req.remoteAddress().toString();
}

bool ValidNpid(const QString& npid) {
    if (npid.size() < 3 || npid.size() > MaxNpidLength)
        return false;
    for (const QChar c : npid) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_'))
            return false;
    }
    return true;
}

bool LooksLikeEmail(const QString& email) {
    const int at = email.indexOf(QLatin1Char('@'));
    return at > 0 && at < email.size() - 1 && !email.contains(QLatin1Char(' ')) &&
           email.size() <= 254;
}

// What an avatar URL is allowed to be.
constexpr int MaxAvatarUrlLength = 500;

bool ValidAvatarUrl(const QString& url) {
    if (url.isEmpty() || url.size() > MaxAvatarUrlLength)
        return false;
    for (const QChar c : url) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
    }
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return false;
    const QString scheme = parsed.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

int PointsForGrade(const QString& grade) {
    if (grade == QLatin1String("P"))
        return 180;
    if (grade == QLatin1String("G"))
        return 90;
    if (grade == QLatin1String("S"))
        return 30;
    if (grade == QLatin1String("B"))
        return 15;
    return 0;
}

bool MemberApiServer::CheckApiKey(const QHttpServerRequest& req) const {
    const QString configured = m_config ? m_config->GetMemberApiKey() : QString();
    if (configured.isEmpty())
        return true; // no key configured, nothing to check
    return SecretsEqual(req.value("X-Member-Api-Key"), configured.toUtf8());
}

QHttpServerResponse MemberApiServer::ApiKeyError(const QHttpServerRequest& req) const {
    const bool absent = req.value("X-Member-Api-Key").isEmpty();
    qWarning().nospace().noquote() << "MemberApi: rejected request from " << PeerOf(req)
                                   << " — key " << (absent ? "not supplied" : "did not match");
    return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_BAD_API_KEY,
                     QStringLiteral("This server does not accept requests from this client."));
}

MemberApiServer::MemberApiServer(QObject* parent) : QObject(parent) {}
MemberApiServer::~MemberApiServer() = default;

bool MemberApiServer::Start(ConfigManager* config, const QString& dbPath, SharedState* shared) {
    m_config = config;
    m_shared = shared;
    m_dbPath = dbPath;

    m_db = std::make_unique<Database>(QStringLiteral("member_api"));
    if (!m_db->Open(dbPath)) {
        qCritical() << "MemberApiServer: failed to open database at" << dbPath;
        return false;
    }

    m_http = std::make_unique<QHttpServer>(this);
    RegisterRoutes();

    m_tcp = std::make_unique<QTcpServer>(this);
    const QString host = m_config->GetMemberApiHost();
    const quint16 port = static_cast<quint16>(m_config->GetMemberApiPort().toUShort());
    const QHostAddress bindAddress = host.isEmpty() ? QHostAddress::LocalHost : QHostAddress(host);

    if (!m_tcp->listen(bindAddress, port)) {
        qCritical() << "MemberApiServer: cannot listen on" << host << port << ":"
                    << m_tcp->errorString();
        return false;
    }
    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "MemberApiServer: cannot bind HTTP server";
        return false;
    }

    qInfo().nospace().noquote() << "Member API listening on " << host << ":" << port;

    if (bindAddress != QHostAddress::LocalHost && bindAddress != QHostAddress::LocalHostIPv6) {
        const QString generated = m_config->EnsureMemberApiKey();
        if (!generated.isEmpty()) {
            qWarning().nospace().noquote()
                << "MemberApiServer: no MemberApiKey was set, so one has been generated and "
                   "saved to shadnet.cfg. Put this in the website's config.php as "
                   "'member_key' — it is printed once:\n    "
                << generated;
        }
    }

    if (bindAddress != QHostAddress::LocalHost && bindAddress != QHostAddress::LocalHostIPv6) {
        qWarning() << "MemberApiServer: bound to a non-loopback address. Put it behind a "
                      "reverse proxy with TLS — it accepts passwords in request bodies.";
    }
    return true;
}

// Sessions

QString MemberApiServer::IssueToken(int64_t userId, const QString& npid) {
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(raw.begin(), raw.end());
    const QString token = QString::fromLatin1(raw.toHex());

    MemberSession session;
    session.userId = userId;
    session.npid = npid;
    session.expires = QDateTime::currentDateTimeUtc().addSecs(SessionMinutes * 60);

    QWriteLocker lk(&m_sessionsLock);
    m_sessions.insert(token, session);
    return token;
}

void MemberApiServer::RevokeToken(const QString& token) {
    QWriteLocker lk(&m_sessionsLock);
    m_sessions.remove(token);
}

void MemberApiServer::RevokeSessionsFor(int64_t userId) {
    QWriteLocker lk(&m_sessionsLock);
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->userId == userId) {
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

void MemberApiServer::PruneExpiredSessions() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QWriteLocker lk(&m_sessionsLock);
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->expires <= now) {
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<MemberApiServer::MemberSession> MemberApiServer::Authenticate(
    const QHttpServerRequest& req) {
    const QString token = BearerToken(req);
    if (token.isEmpty())
        return std::nullopt;

    QWriteLocker lk(&m_sessionsLock);
    auto it = m_sessions.find(token);
    if (it == m_sessions.end())
        return std::nullopt;
    if (it->expires <= QDateTime::currentDateTimeUtc()) {
        m_sessions.erase(it);
        return std::nullopt;
    }
    return *it;
}

// Throttling

bool MemberApiServer::IsThrottled(const QString& peer, int& retryAfterSecs) {
    QWriteLocker lk(&m_failuresLock);
    auto it = m_failures.find(peer);
    if (it == m_failures.end() || it->blockedUntil.isNull())
        return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (it->blockedUntil <= now) {
        m_failures.erase(it);
        return false;
    }
    retryAfterSecs = static_cast<int>(now.secsTo(it->blockedUntil));
    return true;
}

void MemberApiServer::RegisterFailure(const QString& peer) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QWriteLocker lk(&m_failuresLock);
    FailureRecord& r = m_failures[peer];
    if (r.firstAt.isNull() || r.firstAt.secsTo(now) > FailureWindowSeconds) {
        r.firstAt = now;
        r.count = 0;
    }
    if (++r.count >= MaxFailuresPerPeer) {
        r.blockedUntil = now.addSecs(BlockSeconds);
        r.count = 0;
        r.firstAt = QDateTime();
    }
}

void MemberApiServer::ClearFailures(const QString& peer) {
    QWriteLocker lk(&m_failuresLock);
    m_failures.remove(peer);
}

// Routes

void MemberApiServer::RegisterRoutes() {
    // GET /member/v1/status
    m_http->route("/member/v1/status", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      if (!CheckApiKey(req))
                          return ApiKeyError(req);

                      QJsonObject body;
                      body.insert(QStringLiteral("service"), QStringLiteral("shadnet-member"));
                      body.insert(QStringLiteral("registrationOpen"),
                                  m_config && m_config->IsRegistrationAllowed(QString()));
                      // Whether a key is needed, never the key itself.
                      body.insert(QStringLiteral("registrationKeyRequired"),
                                  m_config && m_config->IsRegistrationKeyRequired());
                      return JsonOk(body);
                  });

    // POST /member/v1/register — { npid, email, password, key? }
    m_http->route(
        "/member/v1/register", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const QString peer = PeerOf(req);
            int retryAfter = 0;
            if (IsThrottled(peer, retryAfter)) {
                return JsonError(
                    QHttpServerResponse::StatusCode::TooManyRequests, ERR_TOO_MANY,
                    QStringLiteral("Too many attempts. Try again in %1 seconds.").arg(retryAfter));
            }

            if (!m_config) {
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("Server not configured."));
            }

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            const QString npid = bodyOpt->value(QStringLiteral("npid")).toString().trimmed();
            const QString email = bodyOpt->value(QStringLiteral("email")).toString().trimmed();
            const QString password = bodyOpt->value(QStringLiteral("password")).toString();
            const QString key = bodyOpt->value(QStringLiteral("key")).toString().trimmed();

            if (!ValidNpid(npid)) {
                return JsonError(
                    QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                    QStringLiteral("An NP ID is 3 to %1 characters, using letters, digits, "
                                   "hyphens and underscores.")
                        .arg(MaxNpidLength));
            }
            if (!LooksLikeEmail(email)) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("That does not look like an email address."));
            }
            if (password.size() < MinPasswordLength || password.size() > MaxPasswordLength) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Choose a password of at least %1 characters.")
                                     .arg(MinPasswordLength));
            }

            if (!m_config->IsRegistrationAllowed(key)) {
                RegisterFailure(peer);
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN,
                                 m_config->IsRegistrationKeyRequired()
                                     ? QStringLiteral("That registration key is not correct.")
                                     : QStringLiteral("This server is not accepting new "
                                                      "accounts."));
            }

            const QString defaultAvatar =
                QStringLiteral("https://shadps4.net/shadnet/avatars/default_01.png");
            const auto err = m_db->CreateAccount(npid, password, defaultAvatar, email);
            if (err) {
                switch (*err) {
                case DbError::ExistingUsername:
                    return JsonError(QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                                     QStringLiteral("That NP ID is already taken."));
                case DbError::ExistingEmail:
                    return JsonError(
                        QHttpServerResponse::StatusCode::Conflict, ERR_CONFLICT,
                        QStringLiteral("There is already an account with that email address."));
                case DbError::InvalidEmail:
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     QStringLiteral("The server did not accept that email "
                                                    "address."));
                case DbError::InvalidInput:
                    return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                     QStringLiteral("The server did not accept those details."));
                default:
                    qCritical() << "MemberApi: account creation failed for" << npid << "with error"
                                << static_cast<int>(*err);
                    return JsonError(QHttpServerResponse::StatusCode::InternalServerError,
                                     ERR_INTERNAL,
                                     QStringLiteral("The account could not be created. This is a "
                                                    "problem on the server, not with your "
                                                    "details."));
                }
            }

            qInfo().nospace().noquote() << "MemberApi: registered " << npid;
            QJsonObject body;
            body.insert(QStringLiteral("registered"), true);
            body.insert(QStringLiteral("npid"), npid);
            return JsonOk(body);
        });

    // POST /member/v1/login — { npid, password } -> { token }
    m_http->route(
        "/member/v1/login", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const QString peer = PeerOf(req);
            int retryAfter = 0;
            if (IsThrottled(peer, retryAfter)) {
                return JsonError(
                    QHttpServerResponse::StatusCode::TooManyRequests, ERR_TOO_MANY,
                    QStringLiteral("Too many attempts. Try again in %1 seconds.").arg(retryAfter));
            }

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            const QString npid = bodyOpt->value(QStringLiteral("npid")).toString().trimmed();
            const QString password = bodyOpt->value(QStringLiteral("password")).toString();
            if (npid.isEmpty() || password.isEmpty()) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Give both an NP ID and a password."));
            }

            const auto user = m_db->CheckUser(npid, password, QString(), false);
            if (!user) {
                RegisterFailure(peer);
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("That NP ID and password do not match."));
            }
            if (user->banned) {
                RegisterFailure(peer);
                return JsonError(QHttpServerResponse::StatusCode::Forbidden, ERR_FORBIDDEN,
                                 QStringLiteral("This account is banned."));
            }

            ClearFailures(peer);
            PruneExpiredSessions();
            const QString token = IssueToken(user->userId, user->username);

            qInfo().nospace().noquote() << "MemberApi: " << user->username << " signed in";
            QJsonObject body;
            body.insert(QStringLiteral("token"), token);
            body.insert(QStringLiteral("npid"), user->username);
            body.insert(QStringLiteral("expiresInSeconds"), SessionMinutes * 60);
            return JsonOk(body);
        });

    // POST /member/v1/logout
    m_http->route("/member/v1/logout", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      if (!CheckApiKey(req))
                          return ApiKeyError(req);

                      const QString token = BearerToken(req);
                      if (!token.isEmpty())
                          RevokeToken(token);
                      QJsonObject body;
                      body.insert(QStringLiteral("signedOut"), true);
                      return JsonOk(body);
                  });

    // GET /member/v1/me — the caller's own account.
    m_http->route("/member/v1/me", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                      if (!CheckApiKey(req))
                          return ApiKeyError(req);

                      const auto session = Authenticate(req);
                      if (!session)
                          return JsonError(QHttpServerResponse::StatusCode::Unauthorized,
                                           ERR_UNAUTHORIZED, QStringLiteral("Sign in first."));

                      const auto row = m_db->GetUserRow(session->userId);
                      if (!row)
                          return JsonError(QHttpServerResponse::StatusCode::Unauthorized,
                                           ERR_UNAUTHORIZED,
                                           QStringLiteral("This account no longer exists."));

                      QJsonObject body;
                      body.insert(QStringLiteral("npid"), row->username);
                      body.insert(QStringLiteral("email"), row->email);
                      body.insert(QStringLiteral("creation"), static_cast<qint64>(row->creation));
                      body.insert(QStringLiteral("lastLogin"), static_cast<qint64>(row->lastLogin));
                      body.insert(QStringLiteral("clientVersion"), row->clientVersion);
                      body.insert(QStringLiteral("clientVersionAt"),
                                  static_cast<qint64>(row->clientVersionAt));
                      body.insert(QStringLiteral("avatarUrl"),
                                  m_db->GetAvatarUrl(session->userId).value_or(QString()));
                      return JsonOk(body);
                  });

    // GET /member/v1/me/trophies — the caller's own trophies, with names and
    // points where the game's configuration has been imported.
    m_http->route(
        "/member/v1/me/trophies", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const auto session = Authenticate(req);
            if (!session)
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("Sign in first."));

            QJsonArray games;
            int total = 0, points = 0, completed = 0;
            int bronze = 0, silver = 0, gold = 0, platinum = 0, unknown = 0;

            for (const auto& g : m_db->ListPlayerTrophySummary(session->userId)) {
                QJsonObject o;
                o.insert(QStringLiteral("commid"), g.comId);
                o.insert(QStringLiteral("name"), g.titleName);
                o.insert(QStringLiteral("trophies"), g.trophies);
                o.insert(QStringLiteral("setTotal"), g.totalInGame);
                o.insert(QStringLiteral("completion"),
                         g.totalInGame > 0
                             ? std::round(g.trophies * 10000.0 / g.totalInGame) / 100.0
                             : -1.0);
                QJsonObject grades;
                grades.insert(QStringLiteral("bronze"), g.bronze);
                grades.insert(QStringLiteral("silver"), g.silver);
                grades.insert(QStringLiteral("gold"), g.gold);
                grades.insert(QStringLiteral("platinum"), g.platinum);
                o.insert(QStringLiteral("grades"), grades);
                o.insert(QStringLiteral("unknownGrade"), g.unknownGrade);
                const int gamePoints =
                    g.bronze * 15 + g.silver * 30 + g.gold * 90 + g.platinum * 180;
                o.insert(QStringLiteral("points"), gamePoints);
                o.insert(QStringLiteral("firstEarnedAt"), static_cast<qint64>(g.firstEarnedAt));
                o.insert(QStringLiteral("lastEarnedAt"), static_cast<qint64>(g.lastEarnedAt));
                games.append(o);

                total += g.trophies;
                points += gamePoints;
                bronze += g.bronze;
                silver += g.silver;
                gold += g.gold;
                platinum += g.platinum;
                unknown += g.unknownGrade;
                if (g.totalInGame > 0 && g.trophies >= g.totalInGame)
                    ++completed;
            }

            QJsonObject grades;
            grades.insert(QStringLiteral("bronze"), bronze);
            grades.insert(QStringLiteral("silver"), silver);
            grades.insert(QStringLiteral("gold"), gold);
            grades.insert(QStringLiteral("platinum"), platinum);

            QJsonObject body;
            body.insert(QStringLiteral("npid"), session->npid);
            body.insert(QStringLiteral("total"), total);
            body.insert(QStringLiteral("points"), points);
            body.insert(QStringLiteral("gamesCompleted"), completed);
            body.insert(QStringLiteral("grades"), grades);
            body.insert(QStringLiteral("unknownGrade"), unknown);
            body.insert(QStringLiteral("games"), games);
            return JsonOk(body);
        });

    // GET /member/v1/me/trophies/<comId> — one game, trophy by trophy.
    m_http->route(
        "/member/v1/me/trophies/<arg>", QHttpServerRequest::Method::Get,
        [this](const QString& comId, const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const auto session = Authenticate(req);
            if (!session)
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("Sign in first."));

            QHash<int, Database::TrophyMetaRow> meta;
            for (const auto& m : m_db->ListTrophyMeta(comId))
                meta.insert(m.trophyId, m);

            QJsonArray trophies;
            for (const auto& t : m_db->ListUserTrophiesForGame(session->userId, comId)) {
                QJsonObject o;
                o.insert(QStringLiteral("trophyId"), t.trophyId);
                o.insert(QStringLiteral("earnedAt"), static_cast<qint64>(t.earnedAt));
                const auto it = meta.constFind(t.trophyId);
                if (it != meta.constEnd()) {
                    o.insert(QStringLiteral("name"), it->name);
                    o.insert(QStringLiteral("detail"), it->detail);
                    o.insert(QStringLiteral("grade"), it->grade);
                    o.insert(QStringLiteral("hidden"), it->hidden);
                    o.insert(QStringLiteral("groupId"), it->groupId);
                    o.insert(QStringLiteral("points"), PointsForGrade(it->grade));
                }
                trophies.append(o);
            }

            QJsonObject body;
            body.insert(QStringLiteral("commid"), comId);
            body.insert(QStringLiteral("name"), m_db->GetTitleName(comId).value_or(QString()));
            body.insert(QStringLiteral("trophies"), trophies);
            return JsonOk(body);
        });

    // POST /member/v1/me/avatar — { url }
    m_http->route(
        "/member/v1/me/avatar", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const auto session = Authenticate(req);
            if (!session)
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("Sign in first."));

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            const QString url = bodyOpt->value(QStringLiteral("url")).toString().trimmed();
            if (!ValidAvatarUrl(url)) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("An avatar URL must be an http or https address "
                                                "of at most %1 characters.")
                                     .arg(MaxAvatarUrlLength));
            }

            if (!m_db->SetAvatarUrl(session->userId, url)) {
                qCritical() << "MemberApi: avatar update failed for" << session->npid;
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The picture could not be saved."));
            }

            QJsonObject body;
            body.insert(QStringLiteral("avatarUrl"), url);
            return JsonOk(body);
        });

    // POST /member/v1/me/password — { current, password }
    m_http->route(
        "/member/v1/me/password", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!CheckApiKey(req))
                return ApiKeyError(req);

            const auto session = Authenticate(req);
            if (!session)
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("Sign in first."));

            QString parseError;
            const auto bodyOpt = ParseJsonBody(req, parseError);
            if (!bodyOpt)
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 parseError);

            const QString current = bodyOpt->value(QStringLiteral("current")).toString();
            const QString next = bodyOpt->value(QStringLiteral("password")).toString();

            if (next.size() < MinPasswordLength || next.size() > MaxPasswordLength) {
                return JsonError(QHttpServerResponse::StatusCode::BadRequest, ERR_BAD_REQUEST,
                                 QStringLiteral("Choose a password of at least %1 characters.")
                                     .arg(MinPasswordLength));
            }
            if (!m_db->CheckUser(session->npid, current, QString(), false)) {
                RegisterFailure(PeerOf(req));
                return JsonError(QHttpServerResponse::StatusCode::Unauthorized, ERR_UNAUTHORIZED,
                                 QStringLiteral("That is not your current password."));
            }

            if (!m_db->SetPassword(session->userId, next)) {
                qCritical() << "MemberApi: password change failed for" << session->npid;
                return JsonError(QHttpServerResponse::StatusCode::InternalServerError, ERR_INTERNAL,
                                 QStringLiteral("The password could not be changed."));
            }

            RevokeSessionsFor(session->userId);

            qInfo().nospace().noquote()
                << "MemberApi: " << session->npid << " changed their password";
            QJsonObject body;
            body.insert(QStringLiteral("changed"), true);
            body.insert(QStringLiteral("signedOutEverywhere"), true);
            return JsonOk(body);
        });
}
