// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include "config.h"

class ScoreCache;
struct SharedState;

class StatsServer : public QObject {
    Q_OBJECT
public:
    explicit StatsServer(QObject* parent = nullptr);
    ~StatsServer();

    // scoreCache + shared are owned by ShadNetServer and must outlive this server.
    // dbPath is the SQLite file used for DB-backed stats (e.g. registered-user count).
    bool Start(ConfigManager* config, ScoreCache* scoreCache, const SharedState* shared,
               const QString& dbPath);

private:
    void RegisterRoutes();

    QByteArray CachedOrBuild(const QString& key, const std::function<QByteArray()>& build);

    QByteArray BuildUsageJson() const;
    QByteArray BuildRegisteredJson() const;
    QByteArray BuildComIdScoreJson(const QString& comId) const;
    QByteArray BuildBoardScoreJson(const QString& comId, uint32_t boardId) const;

    ConfigManager* m_config = nullptr;
    ScoreCache* m_scoreCache = nullptr;
    const SharedState* m_shared = nullptr;
    QString m_dbPath;
    int m_cacheLife = 30;
    QString m_path = QStringLiteral("stats");

    std::unique_ptr<QHttpServer> m_http;
    std::unique_ptr<QTcpServer> m_tcp;

    struct CacheEntry {
        QByteArray json;
        QDateTime expiry;
    };
    mutable QMutex m_cacheMutex;
    QHash<QString, CacheEntry> m_cache;
};
