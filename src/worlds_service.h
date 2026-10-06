// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QMutex>
#include <QObject>
#include "matching_types.h"
#include "worldsconfig.h"

struct WorldsResult {
    bool ok = false;
    int status = 500;
    QString message;
    WorldsSnapshot snapshot;
};

class WorldsService : public QObject {
public:
    WorldsService(const QString& path, MatchingSharedState* matching, QObject* parent = nullptr);
    bool Start(QString& error);
    WorldsResult Handle(const QString& operation, const QJsonObject& body);

private:
    void AcceptLocalConnections();
    QString m_path;
    MatchingSharedState* m_matching;
    QLockFile m_runningLock;
    QLocalServer m_local;
    QMutex m_mutex;
    QString m_activeContent;
    QString m_activeRevision;
};
