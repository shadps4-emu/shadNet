// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include "proto_utils.h"
#include "score_cache.h"
#include "score_types.h"

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

static shadnet::ScoreRankData ToRankData(const ScoreEntry& e, int index) {
    shadnet::ScoreRankData r;
    const QByteArray npidU8 = e.npid.toUtf8();
    r.set_npid(npidU8.constData(), static_cast<size_t>(npidU8.size()));
    r.set_pcid(e.characterId);
    r.set_rank(static_cast<uint32_t>(index + 1));
    r.set_score(e.score);
    r.set_hasgamedata(e.dataId.has_value());
    r.set_recorddate(e.timestamp);
    return r;
}

void ScoreCache::LoadTable(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg,
                           const QVector<ScoreEntry>& entries) {
    QWriteLocker lk(&m_lock);
    ScoreTableCache& t = GetOrCreate(comId, boardId, cfg);
    t.config = cfg;
    t.sorted = entries;
    t.lookup.clear();
    for (int i = 0; i < entries.size(); ++i)
        t.lookup[entries[i].userId][entries[i].characterId] = i;
    t.lastInsert = ShadNetTimestamp();
}

uint32_t ScoreCache::InsertScore(const QString& comId, uint32_t boardId,
                                 const ScoreBoardConfig& cfg, const ScoreEntry& entry) {
    QWriteLocker lk(&m_lock);
    ScoreTableCache& t = GetOrCreate(comId, boardId, cfg);
    t.lastInsert = ShadNetTimestamp();

    // Remove any existing entry for this user/character slot.
    std::optional<int> removedAt;
    if (t.lookup.contains(entry.userId) && t.lookup[entry.userId].contains(entry.characterId)) {
        int pos = t.lookup[entry.userId][entry.characterId];
        t.sorted.remove(pos);
        t.lookup[entry.userId].remove(entry.characterId);
        Reindex(t, pos);
        removedAt = pos;
    }

    // Find insertion position (sorted best-first).
    auto it = std::lower_bound(
        t.sorted.begin(), t.sorted.end(), entry,
        [&](const ScoreEntry& a, const ScoreEntry& b) { return RanksBefore(a, b, cfg.sortMode); });
    int insertPos = static_cast<int>(it - t.sorted.begin());

    // Check whether this score makes the board.
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

    // Trim to rank limit.
    if (static_cast<uint32_t>(t.sorted.size()) > cfg.rankLimit) {
        int last = t.sorted.size() - 1;
        t.lookup[t.sorted[last].userId].remove(t.sorted[last].characterId);
        t.sorted.remove(last);
    }

    return static_cast<uint32_t>(insertPos + 1);
}

// Append one rank entry (and optionally comment / game-info) to a response.
static void AppendEntry(shadnet::GetScoreResponse& resp, const ScoreEntry& e, int idx,
                        bool withComment, bool withGameInfo) {
    *resp.add_rankarray() = ToRankData(e, idx);
    if (withComment) {
        const QByteArray commentU8 = e.comment.toUtf8();
        resp.add_commentarray()->assign(commentU8.constData(),
                                        static_cast<size_t>(commentU8.size()));
    }
    if (withGameInfo) {
        auto* info = resp.add_infoarray();
        info->set_data(e.gameInfo.constData(), static_cast<size_t>(e.gameInfo.size()));
    }
}

// Append a blank entry (user not on this board) to keep index alignment.
static void appendBlankEntry(shadnet::GetScoreResponse& resp, bool withComment, bool withGameInfo) {
    resp.add_rankarray(); // default-constructed ScoreRankData (all zeros)
    if (withComment)
        resp.add_commentarray("");
    if (withGameInfo)
        resp.add_infoarray();
}

shadnet::GetScoreResponse ScoreCache::GetScoreRange(const QString& comId, uint32_t boardId,
                                                    uint32_t startRank, uint32_t numRanks,
                                                    bool withComment, bool withGameInfo) {
    shadnet::GetScoreResponse resp;
    QReadLocker lk(&m_lock);

    if (!m_tables.contains(comId) || !m_tables[comId].contains(boardId)) {
        resp.set_lastsortdate(ShadNetTimestamp());
        return resp;
    }

    const ScoreTableCache& t = m_tables[comId][boardId];
    resp.set_totalrecord(static_cast<uint32_t>(t.sorted.size()));
    resp.set_lastsortdate(t.lastInsert);

    if (startRank == 0)
        startRank = 1;
    int start = static_cast<int>(startRank) - 1;
    int end = std::min(start + static_cast<int>(numRanks), static_cast<int>(t.sorted.size()));

    for (int i = start; i < end; ++i)
        AppendEntry(resp, t.sorted[i], i, withComment, withGameInfo);

    return resp;
}

shadnet::GetScoreResponse ScoreCache::GetScoreByIds(const QString& comId, uint32_t boardId,
                                                    const QVector<QPair<int64_t, int32_t>>& ids,
                                                    bool withComment, bool withGameInfo) {
    shadnet::GetScoreResponse resp;
    QReadLocker lk(&m_lock);

    const ScoreTableCache* tp = nullptr;
    if (m_tables.contains(comId) && m_tables[comId].contains(boardId))
        tp = &m_tables[comId][boardId];

    resp.set_totalrecord(tp ? static_cast<uint32_t>(tp->sorted.size()) : 0);
    resp.set_lastsortdate(tp ? tp->lastInsert : ShadNetTimestamp());

    for (const auto& [uid, cid] : ids) {
        if (!tp || !tp->lookup.contains(uid) || !tp->lookup[uid].contains(cid)) {
            appendBlankEntry(resp, withComment, withGameInfo);
            continue;
        }
        int idx = tp->lookup[uid][cid];
        AppendEntry(resp, tp->sorted[idx], idx, withComment, withGameInfo);
    }

    return resp;
}

// helper methods
std::optional<ScoreCacheError> ScoreCache::ContainsScoreWithNoData(const QString& comId,
                                                                   uint32_t boardId, int64_t userId,
                                                                   int32_t charId, int64_t score) {
    QReadLocker lk(&m_lock);
    if (!m_tables.contains(comId) || !m_tables[comId].contains(boardId))
        return ScoreCacheError::NotFound;
    const ScoreTableCache& t = m_tables[comId][boardId];
    if (!t.lookup.contains(userId) || !t.lookup[userId].contains(charId))
        return ScoreCacheError::NotFound;
    const ScoreEntry& e = t.sorted[t.lookup[userId][charId]];
    if (e.score != score)
        return ScoreCacheError::Invalid;
    if (e.dataId.has_value())
        return ScoreCacheError::HasData;
    return std::nullopt;
}

std::optional<ScoreCacheError> ScoreCache::SetGameData(const QString& comId, uint32_t boardId,
                                                       int64_t userId, int32_t charId,
                                                       uint64_t dataId) {
    QWriteLocker lk(&m_lock);
    if (!m_tables.contains(comId) || !m_tables[comId].contains(boardId))
        return ScoreCacheError::NotFound;
    ScoreTableCache& t = m_tables[comId][boardId];
    if (!t.lookup.contains(userId) || !t.lookup[userId].contains(charId))
        return ScoreCacheError::NotFound;
    t.sorted[t.lookup[userId][charId]].dataId = dataId;
    return std::nullopt;
}

std::pair<bool, uint64_t> ScoreCache::GetGameDataId(const QString& comId, uint32_t boardId,
                                                    int64_t userId, int32_t charId) {
    QReadLocker lk(&m_lock);
    if (!m_tables.contains(comId) || !m_tables[comId].contains(boardId))
        return {false, 0};
    const ScoreTableCache& t = m_tables[comId][boardId];
    if (!t.lookup.contains(userId) || !t.lookup[userId].contains(charId))
        return {false, 0};
    const auto& e = t.sorted[t.lookup[userId][charId]];
    if (!e.dataId.has_value())
        return {false, 0};
    return {true, *e.dataId};
}
