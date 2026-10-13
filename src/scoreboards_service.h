// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include "worldsconfig.h"

class Database;
class ScoreCache;

struct ScoreboardsResult {
    bool ok = false;
    int status = 500;
    QString message;
    WorldsSnapshot snapshot;
    int boardsApplied = 0;
};

class ScoreboardsService {
public:
    ScoreboardsService(const QString& path, ScoreCache* cache);
    void Start();
    ScoreboardsResult Handle(const QString& operation, const QJsonObject& body, Database& db);

private:
    int Apply(const QString& content, Database& db);
    QString m_path;
    ScoreCache* m_cache;
    QMutex m_mutex;
    QString m_activeContent;
    QString m_activeRevision;
};
