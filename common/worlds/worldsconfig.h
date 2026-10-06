// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QVector>

struct WorldGroupMapping {
    QString titleId;
    QString group;
    int line = 0;
};

struct WorldDefinition {
    QString group;
    quint32 worldId = 0;
    quint16 serverId = 0;
    quint32 lobbies = 0;
    quint32 maxLobbyMembers = 0;
    int line = 0;
};

struct WorldsConfigIssue {
    int line = 0;
    QString message;
};

struct WorldsConfig {
    QVector<WorldGroupMapping> groups;
    QVector<WorldDefinition> worlds;
    QVector<WorldsConfigIssue> errors;

    bool isValid() const {
        return errors.isEmpty();
    }
};

// Parse the server's [groups] / [worlds] grammar with strict validation.
WorldsConfig parseWorldsConfig(const QString& text);
// Emit the table data in canonical server syntax, preserving row order.
QString serializeWorldsConfig(const WorldsConfig& config);

constexpr qsizetype kMaxWorldsBytes = 512 * 1024;
struct WorldsSnapshot {
    QString content;
    QString revision;
    QString activeContent;
    QString activeRevision;
    bool canReload = false;
    bool exists = true;
};
Q_DECLARE_METATYPE(WorldsSnapshot)

QJsonObject worldsSnapshotJson(const WorldsSnapshot& snapshot);
bool worldsSnapshotFromJson(const QJsonObject& json, WorldsSnapshot& snapshot);
