// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include "worldsconfig.h" // WorldsSnapshot: the same read/save/reload snapshot shape

class ClientVersionRules;
struct SharedState;

struct ClientVersionsResult {
    bool ok = false;
    int status = 500;
    QString message;
    WorldsSnapshot snapshot;
    bool applied = false;
    int rulesApplied = 0;
    int rechecked = 0; // online sessions checked again under the new rules
    int wouldDeny = 0; // of those, how many the new rules turn away
    int cooldownsCleared = 0;
};

class ClientVersionsService {
public:
    ClientVersionsService(const QString& path, ClientVersionRules* rules, SharedState* shared);
    void Start();
    ClientVersionsResult Handle(const QString& operation, const QJsonObject& body);

private:
    void Apply(const QString& content, bool recheckOnline, ClientVersionsResult& result);
    QString m_path;
    ClientVersionRules* m_rules;
    SharedState* m_shared;
    QMutex m_mutex;
    QString m_activeContent;
    QString m_activeRevision;
};
