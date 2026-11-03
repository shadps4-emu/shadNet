// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "score_types.h"
#include "stats_server.h"

#include "version.h"

#include <cmath>

#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QReadLocker>
#include "client_session.h"
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
        o.insert("recorded_at", static_cast<qint64>(ShadNetTimestampToUnix(r.recorddate())));
        o.insert("has_game_data", r.hasgamedata());
        o.insert("account_id", static_cast<qint64>(r.accountid()));
        arr.append(o);
    }
    return arr;
}

void AddAvatars(QJsonArray& ranks, Database& db) {
    QStringList npids;
    for (const auto& v : std::as_const(ranks))
        npids << v.toObject().value("npid").toString();
    const auto avatars = db.GetAvatarUrlsByNpid(npids);
    for (int i = 0; i < ranks.size(); ++i) {
        QJsonObject o = ranks.at(i).toObject();
        o.insert("avatarUrl", avatars.value(o.value("npid").toString()));
        ranks[i] = o;
    }
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

    // GET /<statsPath>/version -> server version and when this binary was built.
    m_http->route(base + "/version",
                  [this](const QHttpServerRequest&) { return jsonResponse(BuildVersionJson()); });

    // GET /<statsPath>/trophies/<comId> -> how widely each trophy has been earned.
    // GET /<path>/trophies -> games with trophy activity, plus server-wide totals
    m_http->route(base + "/trophies", [this](const QHttpServerRequest&) {
        return jsonResponse(
            CachedOrBuild(QStringLiteral("trophylist"), [this] { return BuildTrophyListJson(); }));
    });

    // GET /<path>/trophies/ranking -> the top accounts by trophy points. Registered
    // before /trophies/<comId> so "ranking" is not taken for a communication id.
    m_http->route(base + "/trophies/ranking", [this](const QHttpServerRequest&) {
        return jsonResponse(CachedOrBuild(QStringLiteral("trophyranking"),
                                          [this] { return BuildTrophyRankingJson(); }));
    });

    // GET /<path>/trophies/recent -> the newest unlocks across the server. Registered
    // before /trophies/<comId>, like "ranking".
    m_http->route(base + "/trophies/recent", [this](const QHttpServerRequest&) {
        return jsonResponse(CachedOrBuild(QStringLiteral("trophyrecent"),
                                          [this] { return BuildTrophyRecentJson(); }));
    });

    // GET /<path>/trophies/player/<npid> -> one player's trophy profile.
    m_http->route(base + "/trophies/player/<arg>",
                  [this](const QString& npid, const QHttpServerRequest&) {
                      if (npid.isEmpty() || npid.size() > 16) {
                          return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
                      }
                      const QString key = QStringLiteral("trophyplayer:") + npid;
                      return jsonResponse(
                          CachedOrBuild(key, [this, npid] { return BuildTrophyPlayerJson(npid); }));
                  });

    // GET /<path>/trophies/<comId> -> trophy rarity for a game and its top holders.
    m_http->route(
        base + "/trophies/<arg>", [this](const QString& comId, const QHttpServerRequest&) {
            // Same bounds the score routes use: a com id is 9-12 characters.
            if (comId.size() < 9 || comId.size() > 12) {
                return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
            }
            const QString key = QStringLiteral("trophies:") + comId;
            return jsonResponse(
                CachedOrBuild(key, [this, comId] { return BuildTrophyStatsJson(comId); }));
        });

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

    // GET /<path>/scorelist  -> games (commId, titleName) that have scores
    m_http->route(base + "/scorelist", [this](const QHttpServerRequest&) {
        return jsonResponse(
            CachedOrBuild(QStringLiteral("scorelist"), [this] { return BuildScoreListJson(); }));
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
    Database db(QString{});
    const bool haveDb = db.Open(m_dbPath);
    const QVector<uint32_t> boards = m_scoreCache->ListBoards(comId);
    for (uint32_t boardId : boards) {
        const shadnet::GetScoreResponse resp = m_scoreCache->GetScoreRange(
            comId, boardId, /*startRank=*/1, MaxRanksPerBoard, /*withComment=*/false,
            /*withGameInfo=*/false);
        QJsonObject b;
        b.insert("board_id", static_cast<qint64>(boardId));
        b.insert("total_record", static_cast<qint64>(resp.totalrecord()));
        b.insert("last_sort_date", static_cast<qint64>(resp.lastsortdate()));
        b.insert("sorted_at", static_cast<qint64>(ShadNetTimestampToUnix(resp.lastsortdate())));
        QJsonArray ranks = ranksToJson(resp);
        if (haveDb)
            AddAvatars(ranks, db);
        b.insert("ranks", ranks);
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
    root.insert("sorted_at", static_cast<qint64>(ShadNetTimestampToUnix(resp.lastsortdate())));
    QJsonArray ranks = ranksToJson(resp);
    Database db(QString{});
    if (db.Open(m_dbPath))
        AddAvatars(ranks, db);
    root.insert("ranks", ranks);
    return toJson(root);
}

constexpr int PointsBronze = 15;
constexpr int PointsSilver = 30;
constexpr int PointsGold = 90;
constexpr int PointsPlatinum = 180;

int TrophyPoints(int bronze, int silver, int gold, int platinum) {
    return bronze * PointsBronze + silver * PointsSilver + gold * PointsGold +
           platinum * PointsPlatinum;
}

constexpr int MinPlayersForRarity = 20;

QString RarityBand(double percent) {
    if (percent >= 50.0)
        return QStringLiteral("common");
    if (percent >= 15.0)
        return QStringLiteral("uncommon");
    if (percent > 5.0)
        return QStringLiteral("rare");
    return QStringLiteral("veryrare");
}

QJsonObject GradesJson(int bronze, int silver, int gold, int platinum) {
    QJsonObject g;
    g.insert("bronze", bronze);
    g.insert("silver", silver);
    g.insert("gold", gold);
    g.insert("platinum", platinum);
    return g;
}

QByteArray StatsServer::BuildTrophyStatsJson(const QString& comId) const {
    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for trophy stats";
        root.insert("error", QStringLiteral("db unavailable"));
        return toJson(root);
    }

    const int players = db.CountTrophyPlayers(comId);

    QHash<int, Database::TrophyMetaRow> meta;
    for (const auto& m : db.ListTrophyMeta(comId))
        meta.insert(m.trophyId, m);

    QJsonArray trophies;
    int unlocks = 0;
    for (const auto& e : db.ListTrophyEarners(comId)) {
        QJsonObject o;
        o.insert("trophyId", e.trophyId);
        const auto it = meta.constFind(e.trophyId);
        if (it != meta.constEnd()) {
            o.insert("name", it->name);
            o.insert("detail", it->detail);
            o.insert("grade", it->grade);
            o.insert("hidden", it->hidden);
            o.insert("groupId", it->groupId);
        }
        o.insert("earners", e.earners);
        const double share = players > 0 ? std::round(e.earners * 10000.0 / players) / 100.0 : 0.0;
        o.insert("earnedPercent", share);
        if (players >= MinPlayersForRarity) {
            o.insert("rarity", RarityBand(share));
        }
        trophies.append(o);
        unlocks += e.earners;
    }

    const auto shape = db.GetTrophySetShape(comId);

    QJsonArray top;
    for (const auto& tp : db.ListTopTrophyPlayers(comId, 25)) {
        QJsonObject o;
        o.insert("npid", tp.npid);
        o.insert("avatarUrl", tp.avatarUrl);
        o.insert("trophies", tp.trophies);
        o.insert("lastEarnedAt", static_cast<qint64>(tp.lastEarnedAt));
        o.insert("completion",
                 shape.total > 0 ? std::round(tp.trophies * 10000.0 / shape.total) / 100.0 : -1.0);
        top.append(o);
    }

    QString titleName;
    for (const auto& g : db.ListTrophyGames()) {
        if (g.comId == comId) {
            titleName = g.titleName;
            break;
        }
    }

    root.insert("commid", comId);
    root.insert("name", titleName);
    root.insert("players", players);
    root.insert("distinctTrophies", trophies.size());
    root.insert("setTotal", shape.total);
    root.insert("setGrades", GradesJson(shape.bronze, shape.silver, shape.gold, shape.platinum));
    root.insert("setPoints", TrophyPoints(shape.bronze, shape.silver, shape.gold, shape.platinum));
    root.insert("unlocks", unlocks);
    QJsonArray groups;
    for (const auto& g : db.ListTrophyGroups(comId)) {
        QJsonObject o;
        o.insert("groupId", g.groupId);
        o.insert("name", g.name);
        o.insert("detail", g.detail);
        groups.append(o);
    }

    root.insert("trophies", trophies);
    root.insert("groups", groups);
    root.insert("rarityAvailable", players >= MinPlayersForRarity);
    root.insert("hasTrophyNames", !meta.isEmpty());
    root.insert("topPlayers", top);
    return toJson(root);
}

QByteArray StatsServer::BuildTrophyListJson() const {
    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for trophy list";
        root.insert("error", QStringLiteral("db unavailable"));
        return toJson(root);
    }

    QJsonArray games;
    for (const auto& g : db.ListTrophyGames()) {
        QJsonObject o;
        o.insert("commid", g.comId);
        o.insert("name", g.titleName);
        o.insert("players", g.players);
        o.insert("distinctTrophies", g.trophies);
        o.insert("unlocks", g.unlocks);
        o.insert("hasTrophyNames", db.CountTrophyMeta(g.comId) > 0);
        games.append(o);
    }

    const auto totals = db.GetTrophyTotals();
    QJsonObject t;
    t.insert("players", totals.players);
    t.insert("games", totals.games);
    t.insert("unlocks", totals.unlocks);

    root.insert("totals", t);
    root.insert("games", games);
    return toJson(root);
}

QByteArray StatsServer::BuildTrophyPlayerJson(const QString& npid) const {
    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for trophy profile";
        root.insert("error", QStringLiteral("db unavailable"));
        return toJson(root);
    }

    const auto userId = db.GetUserId(npid);
    if (!userId) {
        root.insert("npid", npid);
        root.insert("total", 0);
        root.insert("games", QJsonArray{});
        return toJson(root);
    }

    QJsonArray games;
    int total = 0;
    int bronze = 0, silver = 0, gold = 0, platinum = 0, unknown = 0, completed = 0;
    for (const auto& g : db.ListPlayerTrophySummary(*userId)) {
        QJsonObject o;
        o.insert("commid", g.comId);
        o.insert("name", g.titleName);
        o.insert("trophies", g.trophies);
        o.insert("firstEarnedAt", static_cast<qint64>(g.firstEarnedAt));
        o.insert("lastEarnedAt", static_cast<qint64>(g.lastEarnedAt));
        o.insert("grades", GradesJson(g.bronze, g.silver, g.gold, g.platinum));
        o.insert("unknownGrade", g.unknownGrade);
        o.insert("points", TrophyPoints(g.bronze, g.silver, g.gold, g.platinum));
        o.insert("setTotal", g.totalInGame);
        o.insert("completion", g.totalInGame > 0
                                   ? std::round(g.trophies * 10000.0 / g.totalInGame) / 100.0
                                   : -1.0);
        games.append(o);

        total += g.trophies;
        bronze += g.bronze;
        silver += g.silver;
        gold += g.gold;
        platinum += g.platinum;
        unknown += g.unknownGrade;
        if (g.totalInGame > 0 && g.trophies >= g.totalInGame)
            ++completed;
    }

    root.insert("npid", db.GetUsername(*userId).value_or(npid));
    root.insert("avatarUrl", db.GetAvatarUrl(*userId).value_or(QString()));
    root.insert("total", total);
    root.insert("grades", GradesJson(bronze, silver, gold, platinum));
    root.insert("unknownGrade", unknown);
    root.insert("points", TrophyPoints(bronze, silver, gold, platinum));
    root.insert("gamesCompleted", completed);
    root.insert("games", games);
    return toJson(root);
}

QByteArray StatsServer::BuildTrophyRankingJson() const {
    constexpr int RankingLimit = 500;

    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for trophy ranking";
        root.insert("error", QStringLiteral("db unavailable"));
        return toJson(root);
    }

    QJsonArray players;
    for (const auto& r : db.ListTrophyRanking(RankingLimit)) {
        QJsonObject o;
        o.insert("npid", r.npid);
        o.insert("avatarUrl", r.avatarUrl);
        o.insert("points", r.points);
        o.insert("trophies", r.trophies);
        o.insert("games", r.games);
        o.insert("gamesCompleted", r.completed);
        o.insert("grades", GradesJson(r.bronze, r.silver, r.gold, r.platinum));
        o.insert("unknownGrade", r.unknownGrade);
        players.append(o);
    }
    root.insert("limit", RankingLimit);
    root.insert("players", players);
    return toJson(root);
}

QByteArray StatsServer::BuildTrophyRecentJson() const {
    constexpr int RecentLimit = 30;

    QJsonObject root;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for recent trophies";
        root.insert("error", QStringLiteral("db unavailable"));
        return toJson(root);
    }

    QJsonArray unlocks;
    for (const auto& r : db.ListRecentTrophies(RecentLimit)) {
        QJsonObject o;
        o.insert("npid", r.npid);
        o.insert("avatarUrl", r.avatarUrl);
        o.insert("commid", r.comId);
        o.insert("game", r.titleName);
        o.insert("trophyId", r.trophyId);
        o.insert("earnedAt", static_cast<qint64>(r.earnedAt));
        o.insert("grade", r.grade);
        o.insert("hidden", r.hidden);
        if (!r.hidden)
            o.insert("name", r.name);
        unlocks.append(o);
    }
    root.insert("unlocks", unlocks);
    return toJson(root);
}

QByteArray StatsServer::BuildVersionJson() const {
    QJsonObject body;
    body.insert(QStringLiteral("version"), ShadNet::Version());
    body.insert(QStringLiteral("buildDate"), ShadNet::BuildDate());
    body.insert(QStringLiteral("buildTime"), ShadNet::BuildTime());
    body.insert(QStringLiteral("buildTimestamp"), ShadNet::BuildTimestamp());
    return toJson(body);
}

QByteArray StatsServer::BuildScoreListJson() const {
    QJsonObject root;
    QJsonArray games;
    Database db(QString{});
    if (!db.Open(m_dbPath)) {
        qWarning() << "StatsServer: cannot open DB for score list";
        root.insert("error", QStringLiteral("db unavailable"));
        root.insert("games", games);
        return toJson(root);
    }
    const auto rows = db.ListScoredGameTitles();
    for (const auto& row : rows) {
        QJsonObject g;
        g.insert("name", row.titleName);
        g.insert("commid", row.comId);
        games.append(g);
    }
    root.insert("games", games);
    return toJson(root);
}
