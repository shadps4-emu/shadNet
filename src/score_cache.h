#pragma once
#include <optional>
#include <QHash>
#include <QPair>
#include <QReadWriteLock>
#include <QVector>
#include "score_types.h"

enum class ScoreCacheError { NotFound, Invalid, HasData };

struct ScoreTableCache {
    QVector<ScoreEntry> sorted;                 // sorted best-first
    QHash<int64_t, QHash<int32_t, int>> lookup; // userId -> charId -> index
    ScoreBoardConfig config;
    uint64_t lastInsert = 0;
};

class ScoreCache {
public:
    // Insert or update a score. Returns 1-based rank, or rankLimit+1 if not on board.
    uint32_t InsertScore(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg,
                         const ScoreEntry& entry);

private:
    static bool RanksBefore(const ScoreEntry& a, const ScoreEntry& b, uint32_t sortMode);
    static void Reindex(ScoreTableCache& t, int from);
    ScoreTableCache& GetOrCreate(const QString& comId, uint32_t boardId,
                                 const ScoreBoardConfig& cfg);
    mutable QReadWriteLock m_lock;
    QHash<QString, QHash<uint32_t, ScoreTableCache>> m_tables;
};
