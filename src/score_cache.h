// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <QHash>
#include <QPair>
#include <QReadWriteLock>
#include <QVector>
#include "score_types.h"
#include "shadnet.pb.h"

enum class ScoreCacheError { NotFound, Invalid, HasData };

struct ScoreTableCache {
    QVector<ScoreEntry> sorted;                 // sorted best-first
    QHash<int64_t, QHash<int32_t, int>> lookup; // userId -> charId -> index
    ScoreBoardConfig config;
    uint64_t lastInsert = 0;
};

class ScoreCache {
public:
    void LoadTable(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg,
                   const QVector<ScoreEntry>& entries);

    // Insert or update a score. Returns 1-based rank, or rankLimit+1 if not on board.
    uint32_t InsertScore(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg,
                         const ScoreEntry& entry);

    shadnet::GetScoreResponse GetScoreRange(const QString& comId, uint32_t boardId,
                                            uint32_t startRank, uint32_t numRanks, bool withComment,
                                            bool withGameInfo);

    shadnet::GetScoreResponse GetScoreByIds(const QString& comId, uint32_t boardId,
                                            const QVector<QPair<int64_t, int32_t>>& ids,
                                            bool withComment, bool withGameInfo);

    std::optional<ScoreCacheError> ContainsScoreWithNoData(const QString& comId, uint32_t boardId,
                                                           int64_t userId, int32_t charId,
                                                           int64_t score);

    std::optional<ScoreCacheError> SetGameData(const QString& comId, uint32_t boardId,
                                               int64_t userId, int32_t charId, uint64_t dataId);

    std::pair<bool, uint64_t> GetGameDataId(const QString& comId, uint32_t boardId, int64_t userId,
                                            int32_t charId);

private:
    static bool RanksBefore(const ScoreEntry& a, const ScoreEntry& b, uint32_t sortMode);
    static void Reindex(ScoreTableCache& t, int from);
    ScoreTableCache& GetOrCreate(const QString& comId, uint32_t boardId,
                                 const ScoreBoardConfig& cfg);

    mutable QReadWriteLock m_lock;
    QHash<QString, QHash<uint32_t, ScoreTableCache>> m_tables;
};
