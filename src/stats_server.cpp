// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "stats_server.h"

#include <QDebug>
#include <QHostAddress>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QReadLocker>
#include "client_session.h" // SharedState
#include "database.h"
#include "score_cache.h"

namespace {

constexpr uint32_t MaxRanksPerBoard = 100; // cap leaderboard rows emitted per board

QByteArray toJson(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QHttpServerResponse jsonResponse(const QByteArray& body) {
    return QHttpServerResponse{"application/json", body, QHttpServerResponse::StatusCode::Ok};
}

// Serialize a GetScoreResponse's rank array to JSON (best-first, as cached).
QJsonArray ranksToJson(const shadnet::GetScoreResponse& resp) {
    QJsonArray arr;
    for (int i = 0; i < resp.rankarray_size(); ++i) {
        const auto& r = resp.rankarray(i);
        QJsonObject o;
        o.insert("rank", static_cast<qint64>(r.rank()));
        o.insert("npid", QString::fromStdString(r.npid()));
        o.insert("pc_id", r.pcid());
        o.insert("score", static_cast<qint64>(r.score()));
        o.insert("record_date", static_cast<qint64>(r.recorddate()));
        o.insert("has_game_data", r.hasgamedata());
        o.insert("account_id", static_cast<qint64>(r.accountid()));
        arr.append(o);
    }
    return arr;
}

} // namespace

StatsServer::StatsServer(QObject* parent) : QObject(parent) {}
StatsServer::~StatsServer() = default;

bool StatsServer::Start(ConfigManager* config, ScoreCache* scoreCache, const SharedState* shared,
                        const QString& dbPath) {
    m_config = config;
    m_scoreCache = scoreCache;
    m_shared = shared;
    m_dbPath = dbPath;
    m_cacheLife = config->GetStatsCacheLife();
    m_path = config->GetStatsPath();

    m_http = std::make_unique<QHttpServer>(this);
    RegisterRoutes();

    m_tcp = std::make_unique<QTcpServer>(this);
    const QString host = m_config->GetHost();
    const quint16 port = m_config->GetStatsPort().toUShort();
    if (!m_tcp->listen(QHostAddress(host), port)) {
        qCritical() << "StatsServer: failed to bind" << host << ":" << port << ""
                    << m_tcp->errorString();
        return false;
    }
    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "StatsServer: QHttpServer failed to attach to listener";
        return false;
    }

    qInfo() << "StatsServer listening on" << host << ":" << port << "path /" + m_path;
    return true;
}

void StatsServer::RegisterRoutes() {
    const QString base = QStringLiteral("/") + m_path;

    // GET /<path>/usage
    m_http->route(base + "/usage", [this](const QHttpServerRequest&) {
        return jsonResponse(
            CachedOrBuild(QStringLiteral("usage"), [this] { return BuildUsageJson(); }));
    });

    // GET /<path>/registered  -> total registered accounts in the DB
    m_http->route(base + "/registered", [this](const QHttpServerRequest&) {
        return jsonResponse(
            CachedOrBuild(QStringLiteral("registered"), [this] { return BuildRegisteredJson(); }));
    });

    // GET /<path>/score/<comId>
    m_http->route(base + "/score/<arg>", [this](const QString& comId, const QHttpServerRequest&) {
        if (comId.size() < 9 || comId.size() > 12) {
            return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
        }
        const QString key = QStringLiteral("score:") + comId;
        return jsonResponse(
            CachedOrBuild(key, [this, comId] { return BuildComIdScoreJson(comId); }));
    });

    // GET /<path>/score/<comId>/<boardId>
    m_http->route(base + "/score/<arg>/<arg>", [this](const QString& comId, const QString& boardStr,
                                                      const QHttpServerRequest&) {
        bool ok = false;
        const uint boardId = boardStr.toUInt(&ok);
        if (!ok || comId.size() < 9 || comId.size() > 12) {
            return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
        }
        const QString key = QStringLiteral("board:") + comId + QStringLiteral(":") + boardStr;
        return jsonResponse(CachedOrBuild(
            key, [this, comId, boardId] { return BuildBoardScoreJson(comId, boardId); }));
    });

    m_http->setMissingHandler(
        this, [](const QHttpServerRequest& req, QHttpServerResponder& responder) {
            qWarning() << "Stats: unhandled" << req.method() << req.url().path();
            QJsonObject body;
            body.insert("error", QStringLiteral("not found"));
            responder.sendResponse(QHttpServerResponse{"application/json", toJson(body),
                                                       QHttpServerResponder::StatusCode::NotFound});
        });
}

QByteArray StatsServer::CachedOrBuild(const QString& key,
                                      const std::function<QByteArray()>& build) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    {
        QMutexLocker lk(&m_cacheMutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end() && it->expiry > now) {
            return it->json;
        }
    }
    QByteArray json = build();
    {
        QMutexLocker lk(&m_cacheMutex);
        m_cache[key] = CacheEntry{json, now.addSecs(m_cacheLife)};
    }
    return json;
}

QByteArray StatsServer::BuildRegisteredJson() const {
    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for registered-user count";
        root.insert("error", QStringLiteral("db unavailable"));
        root.insert("registered_users", 0);
        return toJson(root);
    }
    root.insert("registered_users", db.TotalUsers());
    return toJson(root);
}

QByteArray StatsServer::BuildUsageJson() const {
    QJsonObject root;
    QJsonArray games;
    int total = 0;
    {
        QReadLocker lk(&m_shared->usageLock);
        total = m_shared->usageTotalOnline;
        for (auto it = m_shared->usageGameUsers.constBegin();
             it != m_shared->usageGameUsers.constEnd(); ++it) {
            if (it.value() <= 0) {
                continue;
            }
            QJsonObject g;
            g.insert("com_id", it.key());
            g.insert("num_users", it.value());
            games.append(g);
        }
    }
    root.insert("total_users", total);
    root.insert("psn_games", games);
    return toJson(root);
}

QByteArray StatsServer::BuildComIdScoreJson(const QString& comId) const {
    QJsonObject root;
    root.insert("com_id", comId);
    QJsonArray boardsArr;
    const QVector<uint32_t> boards = m_scoreCache->ListBoards(comId);
    for (uint32_t boardId : boards) {
        const shadnet::GetScoreResponse resp = m_scoreCache->GetScoreRange(
            comId, boardId, /*startRank=*/1, MaxRanksPerBoard, /*withComment=*/false,
            /*withGameInfo=*/false);
        QJsonObject b;
        b.insert("board_id", static_cast<qint64>(boardId));
        b.insert("total_record", static_cast<qint64>(resp.totalrecord()));
        b.insert("last_sort_date", static_cast<qint64>(resp.lastsortdate()));
        b.insert("ranks", ranksToJson(resp));
        boardsArr.append(b);
    }
    root.insert("boards", boardsArr);
    return toJson(root);
}

QByteArray StatsServer::BuildBoardScoreJson(const QString& comId, uint32_t boardId) const {
    const shadnet::GetScoreResponse resp =
        m_scoreCache->GetScoreRange(comId, boardId, /*startRank=*/1, MaxRanksPerBoard,
                                    /*withComment=*/false, /*withGameInfo=*/false);
    QJsonObject root;
    root.insert("com_id", comId);
    root.insert("board_id", static_cast<qint64>(boardId));
    root.insert("total_record", static_cast<qint64>(resp.totalrecord()));
    root.insert("last_sort_date", static_cast<qint64>(resp.lastsortdate()));
    root.insert("ranks", ranksToJson(resp));
    return toJson(root);
}
