// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QTextStream>
#include <QThread>
#ifdef Q_OS_WIN
// clang-format off
#include <winsock2.h>
#include <mstcpip.h>
// clang-format on
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif
#include "database.h"
#include "score_db.h"
#include "server.h"

ShadNetServer::ShadNetServer(QObject* parent)
    : QObject(parent), m_unsecuredServer(new QTcpServer(this)) {
    connect(m_unsecuredServer, &QTcpServer::newConnection, this,
            &ShadNetServer::OnNewUnsecuredConnection);
}

ShadNetServer::~ShadNetServer() {
    Stop();
}

bool ShadNetServer::Start(ConfigManager* config) {
    m_config = config;
    m_shared.config = config;

    m_dbPath = "db/shadnet.db";
    QDir().mkpath("db");

    Database schemaInit(QStringLiteral("shadnet_schema_init"));
    if (!schemaInit.Open(m_dbPath)) {
        qCritical() << "Start: DB schema initialisation failed";
        return false;
    }

    // Score subsystem should run after DB path is set.
    if (!InitScoreSystem()) {
        qCritical() << "Score subsystem failed to initialise";
        return false;
    }

    LoadWorldsCfg("worlds.cfg");

    QHostAddress addr;
    const QString host = config->GetHost();
    if (host.isEmpty() || host == "0.0.0.0")
        addr = QHostAddress::AnyIPv4;
    else
        addr = QHostAddress(host);

    uint16_t plainPort = static_cast<uint16_t>(config->GetUnsecuredPort().toUInt());
    if (m_unsecuredServer->listen(addr, plainPort)) {
        qInfo().nospace().noquote()
            << "Unsecured TCP listener on: " << addr.toString() << ":" << plainPort
            << ". No encryption, use it only on trusted networks";
    } else {
        qCritical() << "Plain TCP listen failed:" << m_unsecuredServer->errorString();
    }

    // Start STUN/signaling UDP server
    m_stunServer = new StunServer(&m_shared, this);
    uint16_t udpPort = static_cast<uint16_t>(config->GetMatchingUdpPort().toUInt());
    if (!m_stunServer->Start(addr, udpPort))
        qWarning() << "STUN UDP listen failed on port" << udpPort;

    // Read-only stats HTTP server (live usage + public leaderboards), own port.
    if (config->IsStatsEnabled()) {
        m_statsServer = std::make_unique<StatsServer>(this);
        if (!m_statsServer->Start(config, m_scoreCache.get(), &m_shared, m_dbPath)) {
            qWarning() << "StatsServer failed to start; continuing without stats";
            m_statsServer.reset();
        }
    }

    return true;
}

void ShadNetServer::Stop() {
    if (m_unsecuredServer->isListening())
        m_unsecuredServer->close();
}

// Score subsystem init

bool ShadNetServer::InitScoreSystem() {
    m_scoreCache = std::make_unique<ScoreCache>();
    m_scoreFiles = std::make_unique<ScoreFiles>();
    m_shared.scoreCache = m_scoreCache.get();
    m_shared.scoreFiles = m_scoreFiles.get();

    if (!m_scoreFiles->Init())
        return false;

    // Use a dedicated connection for server-startup DB work.
    const QString connName = QStringLiteral("shadnet_server_init");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(m_dbPath);
        if (!db.open()) {
            qCritical() << "InitScoreSystem: cannot open DB:" << db.lastError().text();
            return false;
        }

        ScoreDb sdb(db);

        // Load scoreboards.cfg board configs before populating the cache.
        LoadScoreboardsCfg("scoreboards.cfg");

        // Pre-populate the in-memory cache from every board row in the DB.
        const auto boards = sdb.AllBoards();
        for (const auto& [key, cfg] : boards) {
            auto scores = sdb.ScoresForBoard(key.first, key.second, cfg);
            m_scoreCache->LoadTable(key.first, key.second, cfg, scores);
        }
        qInfo() << "Score cache loaded:" << boards.size() << "board(s)";

        // Delete .sdt files whose data_id is no longer referenced.
        const auto dbIds = sdb.AllDataIds();
        QSet<uint64_t> validSet(dbIds.begin(), dbIds.end());
        m_scoreFiles->CleanOrphans(validSet);

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return true;
}

// scoreboards.cfg format — one board definition per line, '#' = comment:
//   COMMUNICATION_ID | TABLE_IDS | RANK_LIMIT | UPDATE_MODE | SORT_MODE | UPLOAD_NUM | UPLOAD_SIZE
//
// UPDATE_MODE: NORMAL_UPDATE | FORCE_UPDATE
// SORT_MODE:   DESCENDING_ORDER | ASCENDING_ORDER
//
// Example:
//   NPWR12345_00 | 1,2,3 | 100 | NORMAL_UPDATE | DESCENDING_ORDER | 10 | 6000000
bool ShadNetServer::LoadScoreboardsCfg(const QString& path) {
    QFile f(path);
    if (!f.exists()) {
        qInfo() << "No scoreboards.cfg found,boards will be created on first access";
        return true;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Cannot open scoreboards.cfg:" << f.errorString();
        return false;
    }

    ScoreDb sdb(QSqlDatabase::database("shadnet_server_init"));
    QTextStream ts(&f);
    int lineNo = 0;

    while (!ts.atEnd()) {
        ++lineNo;
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        auto parseLine = [&]() -> bool {
            const QStringList parts = line.split('|');
            if (parts.size() != 7)
                return false;

            const QString comId = parts[0].trimmed();
            const QStringList tableStrs = parts[1].trimmed().split(',');
            ScoreBoardConfig cfg;
            bool ok = false;

            cfg.rankLimit = parts[2].trimmed().toUInt(&ok);
            if (!ok)
                return false;

            const QString um = parts[3].trimmed();
            if (um == "NORMAL_UPDATE")
                cfg.updateMode = 0;
            else if (um == "FORCE_UPDATE")
                cfg.updateMode = 1;
            else
                return false;

            const QString sm = parts[4].trimmed();
            if (sm == "DESCENDING_ORDER")
                cfg.sortMode = 0;
            else if (sm == "ASCENDING_ORDER")
                cfg.sortMode = 1;
            else
                return false;

            cfg.uploadNumLimit = parts[5].trimmed().toUInt(&ok);
            if (!ok)
                return false;
            cfg.uploadSizeLimit = parts[6].trimmed().toULongLong(&ok);
            if (!ok)
                return false;

            for (const QString& ts2 : tableStrs) {
                uint32_t tid = ts2.trimmed().toUInt(&ok);
                if (!ok)
                    return false;
                sdb.SetBoard(comId, tid, cfg);
            }
            return true;
        };

        if (!parseLine())
            qWarning() << "scoreboards.cfg line" << lineNo << "invalid:" << line;
    }

    qInfo() << "scoreboards.cfg loaded";
    return true;
}

bool ShadNetServer::LoadWorldsCfg(const QString& path) {
    QFile f(path);
    if (!f.exists()) {
        qInfo() << "No worlds.cfg found, GetWorldInfoList will use a default world list";
        return true;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Cannot open worlds.cfg:" << f.errorString();
        return false;
    }

    QWriteLocker lk(&m_shared.matching.roomsLock);
    QTextStream ts(&f);
    int lineNo = 0;
    enum class Section { None, Groups, Worlds } section = Section::None;

    auto parseGroups = [&](const QString& line) -> bool {
        const int eq = line.indexOf('=');
        if (eq < 0)
            return false;
        const QString titleId = line.left(eq).trimmed();
        const QString group = line.mid(eq + 1).trimmed();
        if (titleId.isEmpty() || group.isEmpty())
            return false;
        m_shared.matching.titleGroups[titleId] = group;
        return true;
    };

    auto parseWorlds = [&](const QString& line) -> bool {
        const QStringList parts = line.split('|');
        if (parts.size() != 5)
            return false;
        const QString group = parts[0].trimmed();
        if (group.isEmpty())
            return false;
        bool ok = false;
        WorldConfig wc;
        wc.worldId = parts[1].trimmed().toUInt(&ok);
        if (!ok || wc.worldId == 0)
            return false;
        wc.serverId = static_cast<uint16_t>(parts[2].trimmed().toUInt(&ok));
        if (!ok)
            return false;
        wc.lobbiesNum = parts[3].trimmed().toUInt(&ok);
        if (!ok)
            return false;
        wc.maxLobbyMembersNum = parts[4].trimmed().toUInt(&ok);
        if (!ok)
            return false;
        m_shared.matching.worldConfigs[group].append(wc);
        return true;
    };

    while (!ts.atEnd()) {
        ++lineNo;
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        if (line.startsWith('[') && line.endsWith(']')) {
            const QString name = line.mid(1, line.size() - 2).trimmed().toLower();
            if (name == "groups")
                section = Section::Groups;
            else if (name == "worlds")
                section = Section::Worlds;
            else {
                section = Section::None;
                qWarning() << "worlds.cfg line" << lineNo << "unknown section:" << line;
            }
            continue;
        }

        bool ok = false;
        if (section == Section::Groups)
            ok = parseGroups(line);
        else if (section == Section::Worlds)
            ok = parseWorlds(line);

        if (!ok)
            qWarning() << "worlds.cfg line" << lineNo << "invalid:" << line;
    }

    qInfo() << "worlds.cfg loaded:" << m_shared.matching.titleGroups.size() << "group mapping(s),"
            << m_shared.matching.worldConfigs.size() << "configured group(s)";
    return true;
}

static void SetAggressiveKeepalive(QTcpSocket* socket) {
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
#ifdef Q_OS_WIN
    struct tcp_keepalive ka;
    ka.onoff = 1;
    ka.keepalivetime = 60000;    // 60s idle before first probe
    ka.keepaliveinterval = 5000; // 5s between probes (3 probes = ~75s total)
    DWORD bytes = 0;
    WSAIoctl(static_cast<SOCKET>(socket->socketDescriptor()), SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
             nullptr, 0, &bytes, nullptr, nullptr);
#else
    int fd = socket->socketDescriptor();
    int idle = 60, interval = 5, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

void ShadNetServer::SpawnSession(QTcpSocket* socket, bool isSsl) {
    SetAggressiveKeepalive(socket);
    QThread* thread = new QThread;
    ClientSession* session = new ClientSession(socket, &m_shared, m_dbPath, isSsl);
    session->moveToThread(thread);

    connect(thread, &QThread::started, session, &ClientSession::Start);
    connect(session, &ClientSession::Disconnected, thread, &QThread::quit);
    connect(session, &ClientSession::Disconnected, session, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

void ShadNetServer::OnNewUnsecuredConnection() {
    while (m_unsecuredServer->hasPendingConnections()) {
        QTcpSocket* sock = m_unsecuredServer->nextPendingConnection();
        // Disconnect from the server's parent so spawnSession can reparent it
        sock->setParent(nullptr);
        SpawnSession(sock, /*isSsl=*/false);
    }
}
