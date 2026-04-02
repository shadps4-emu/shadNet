#include "score_cache.h"

// help functions
bool ScoreCache::RanksBefore(const ScoreEntry& a, const ScoreEntry& b, uint32_t sortMode) {
    if (a.score != b.score)
        return (sortMode == 0) ? (a.score > b.score) : (a.score < b.score);
    if (a.timestamp != b.timestamp)
        return a.timestamp < b.timestamp;
    return a.userId < b.userId;
}

void ScoreCache::Reindex(ScoreTableCache& t, int from) {
    for (int i = from, n = t.sorted.size(); i < n; ++i)
        t.lookup[t.sorted[i].userId][t.sorted[i].characterId] = i;
}

ScoreTableCache& ScoreCache::GetOrCreate(const QString& comId, uint32_t boardId,
                                         const ScoreBoardConfig& cfg) {
    auto& inner = m_tables[comId];
    if (!inner.contains(boardId)) {
        ScoreTableCache t;
        t.config = cfg;
        inner.insert(boardId, t);
    }
    return inner[boardId];
}

// implementations
uint32_t ScoreCache::InsertScore(const QString& comId, uint32_t boardId,
                                 const ScoreBoardConfig& cfg, const ScoreEntry& entry) {
    QWriteLocker lk(&m_lock);
    ScoreTableCache& t = GetOrCreate(comId, boardId, cfg);
    t.lastInsert = ShadNetTimestamp();

    // Remove existing entry for this user/char if present
    std::optional<int> removedAt;
    if (t.lookup.contains(entry.userId) && t.lookup[entry.userId].contains(entry.characterId)) {
        int pos = t.lookup[entry.userId][entry.characterId];
        t.sorted.remove(pos);
        t.lookup[entry.userId].remove(entry.characterId);
        Reindex(t, pos);
        removedAt = pos;
    }

    auto it = std::lower_bound(
        t.sorted.begin(), t.sorted.end(), entry,
        [&](const ScoreEntry& a, const ScoreEntry& b) { return RanksBefore(a, b, cfg.sortMode); });
    int insertPos = static_cast<int>(it - t.sorted.begin());

    bool fits = (static_cast<uint32_t>(t.sorted.size()) < cfg.rankLimit) ||
                (insertPos < static_cast<int>(cfg.rankLimit));
    if (!fits)
        return cfg.rankLimit + 1;

    t.sorted.insert(insertPos, entry);
    t.lookup[entry.userId][entry.characterId] = insertPos;
    int reindexFrom = insertPos + 1;
    if (removedAt.has_value())
        reindexFrom = std::min(reindexFrom, *removedAt);
    Reindex(t, reindexFrom);

    if (static_cast<uint32_t>(t.sorted.size()) > cfg.rankLimit) {
        int last = t.sorted.size() - 1;
        t.lookup[t.sorted[last].userId].remove(t.sorted[last].characterId);
        t.sorted.remove(last);
    }
    return static_cast<uint32_t>(insertPos + 1);
}
