// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <QList>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QTcpServer>
#include "client_session.h"
#include "config.h"
#include "score_cache.h"
#include "score_files.h"
#include "stun_server.h"

class ShadNetServer : public QObject {
    Q_OBJECT
public:
    explicit ShadNetServer(QObject* parent = nullptr);
    ~ShadNetServer();

    bool Start(ConfigManager* config);
    void Stop();

private slots:
    void OnNewUnsecuredConnection();

private:
    void SpawnSession(QTcpSocket* socket, bool isSsl);
    bool InitScoreSystem();
    bool LoadScoreboardsCfg(const QString& path);

    ConfigManager* m_config = nullptr;
    QTcpServer* m_unsecuredServer = nullptr; // plain TCP connections
    SharedState m_shared;
    QString m_dbPath;

    std::unique_ptr<ScoreCache> m_scoreCache;
    std::unique_ptr<ScoreFiles> m_scoreFiles;
    StunServer* m_stunServer = nullptr;
};
