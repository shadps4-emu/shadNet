// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QTextStream>
#include <QThread>
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

    // Score subsystem should run after DB path is set.
    if (!InitScoreSystem()) {
        qCritical() << "Score subsystem failed to initialise";
        return false;
    }

    QHostAddress addr;
    const QString host = config->GetHost();
    if (host.isEmpty() || host == "0.0.0.0")
        addr = QHostAddress::AnyIPv4;
    else
        addr = QHostAddress(host);

    // Start STUN/signaling UDP server
    m_stunServer = new StunServer(&m_shared, this);
    uint16_t udpPort = static_cast<uint16_t>(config->GetMatchingUdpPort().toUInt());
    if (!m_stunServer->Start(addr, udpPort))
        qWarning() << "STUN UDP listen failed on port" << udpPort;

    uint16_t plainPort = static_cast<uint16_t>(config->GetUnsecuredPort().toUInt());
    if (m_unsecuredServer->listen(addr, plainPort)) {
        qInfo().nospace() << "Unsesured TCP listener on " << addr.toString() << ":" << plainPort
                          << " No encryption use it only on trusted networks";
    } else {
        qCritical() << "Plain TCP listen failed:" << m_unsecuredServer->errorString();
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
            cfg.uploadSizeLimit = parts[6].trimmed().toUInt(&ok);
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

void ShadNetServer::SpawnSession(QTcpSocket* socket, bool isSsl) {
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
