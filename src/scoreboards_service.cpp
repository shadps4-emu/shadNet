// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "scoreboards_service.h"

#include <QDebug>
#include "database.h"
#include "score_cache.h"
#include "score_db.h"
#include "scoreboardsconfig.h"
#include "worldsstore.h"

namespace {

const QString kLabel = QStringLiteral("scoreboards.cfg");

QString Validate(const QString& content) {
    const auto parsed = parseScoreboardsConfig(content);
    return parsed.isValid() ? QString()
                            : QStringLiteral("Line %1: %2")
                                  .arg(parsed.errors.first().line)
                                  .arg(parsed.errors.first().message);
}

} // namespace

ScoreboardsService::ScoreboardsService(const QString& path, ScoreCache* cache)
    : m_path(worldsFilePath(path)), m_cache(cache) {}

void ScoreboardsService::Start() {
    WorldsFileTransaction transaction(m_path, kLabel, Validate);
    WorldsSnapshot disk;
    QString error;
    if (transaction.read(disk, error)) {
        m_activeContent = disk.content;
        m_activeRevision = disk.revision;
    } else {
        qWarning() << "scoreboards.cfg editor:" << error;
    }
}

int ScoreboardsService::Apply(const QString& content, Database& db) {
    const auto parsed = parseScoreboardsConfig(content);
    ScoreDb sdb(db.Conn());
    int applied = 0;
    for (const auto& e : parsed.entries) {
        ScoreBoardConfig cfg;
        cfg.rankLimit = e.rankLimit;
        cfg.updateMode = e.forceUpdate ? 1 : 0;
        cfg.sortMode = e.ascending ? 1 : 0;
        cfg.uploadNumLimit = e.uploadNumLimit;
        cfg.uploadSizeLimit = e.uploadSizeLimit;
        for (quint32 board : e.boardIds) {
            if (!sdb.SetBoard(e.comId, board, cfg))
                continue;
            if (m_cache)
                m_cache->LoadTable(e.comId, board, cfg, sdb.ScoresForBoard(e.comId, board, cfg));
            ++applied;
        }
    }
    return applied;
}

ScoreboardsResult ScoreboardsService::Handle(const QString& operation, const QJsonObject& body,
                                             Database& db) {
    QMutexLocker serialize(&m_mutex);
    ScoreboardsResult result;
    if (operation != QLatin1String("read") && operation != QLatin1String("save") &&
        operation != QLatin1String("reload")) {
        result.status = 400;
        result.message = QStringLiteral("Unknown scoreboards operation.");
        return result;
    }
    const bool saving = operation == QLatin1String("save");
    const bool mutation = operation != QLatin1String("read");
    if (mutation && (!body.value(QStringLiteral("revision")).isString() ||
                     body.value(QStringLiteral("revision")).toString().isEmpty())) {
        result.status = 400;
        result.message = QStringLiteral("A saved-file revision is required.");
        return result;
    }
    if (saving && (!body.value(QStringLiteral("content")).isString() ||
                   !body.value(QStringLiteral("reload")).isBool())) {
        result.status = 400;
        result.message = QStringLiteral("Save requires string content and a boolean reload flag.");
        return result;
    }
    WorldsFileTransaction transaction(m_path, kLabel, Validate);
    WorldsSnapshot disk;
    if (!transaction.read(disk, result.message))
        return result;
    if (!mutation) {
        disk.activeContent = m_activeContent;
        disk.activeRevision = m_activeRevision.isEmpty() ? disk.revision : m_activeRevision;
        disk.canReload = true;
        result.ok = true;
        result.status = 200;
        result.snapshot = disk;
        return result;
    }
    if (disk.revision != body.value(QStringLiteral("revision")).toString()) {
        result.status = 409;
        result.message = QStringLiteral("scoreboards.cfg changed since it was read. Read the "
                                        "latest version before applying changes.");
        return result;
    }
    const bool apply = !saving || body.value(QStringLiteral("reload")).toBool();
    const QString content =
        saving ? body.value(QStringLiteral("content")).toString() : disk.content;
    if (const QString invalid = Validate(content); !invalid.isEmpty()) {
        result.status = 422;
        result.message = invalid;
        return result;
    }
    if (saving && !transaction.write(content, disk.revision, disk, result.message, result.status))
        return result;
    if (apply) {
        result.boardsApplied = Apply(content, db);
        m_activeContent = content;
        m_activeRevision = disk.revision;
        qInfo() << "scoreboards.cfg applied:" << result.boardsApplied << "board(s)";
    }
    disk.activeContent = m_activeContent;
    disk.activeRevision = m_activeRevision.isEmpty() ? disk.revision : m_activeRevision;
    disk.canReload = true;
    result.ok = true;
    result.status = 200;
    result.snapshot = disk;
    return result;
}
