// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <QSqlDatabase>
#include <QVector>
#include "score_types.h"

enum class ScoreDbError { Internal, NotBest, Invalid };

class ScoreDb {
public:
    explicit ScoreDb(const QSqlDatabase& db) : m_db(db) {}

    // Board configuration
    bool SetBoard(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg);
    std::optional<ScoreBoardConfig> GetBoard(const QString& comId, uint32_t boardId,
                                             bool createMissing);
    QVector<std::pair<QPair<QString, uint32_t>, ScoreBoardConfig>> AllBoards();

    // Score operations
    std::optional<ScoreDbError> RecordScore(const QString& comId, uint32_t boardId,
                                            const ScoreBoardConfig& cfg, const ScoreEntry& entry);
    std::optional<ScoreDbError> SetScoreDataId(const QString& comId, uint32_t boardId,
                                               int64_t userId, int32_t characterId, int64_t score,
                                               uint64_t dataId);
    QVector<ScoreEntry> ScoresForBoard(const QString& comId, uint32_t boardId,
                                       const ScoreBoardConfig& cfg);
    QVector<uint64_t> AllDataIds();
    std::optional<int64_t> UserIdForNpid(const QString& npid);

private:
    bool CreateBoard(const QString& comId, uint32_t boardId);
    QSqlDatabase m_db;
};
