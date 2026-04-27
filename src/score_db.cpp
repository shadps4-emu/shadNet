// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include "score_db.h"

bool ScoreDb::CreateBoard(const QString& comId, uint32_t boardId) {
    ScoreBoardConfig def;
    return SetBoard(comId, boardId, def);
}

bool ScoreDb::SetBoard(const QString& comId, uint32_t boardId, const ScoreBoardConfig& cfg) {
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO score_table(communication_id,board_id,rank_limit,update_mode,"
              "sort_mode,upload_num_limit,upload_size_limit) VALUES(?,?,?,?,?,?,?)"
              " ON CONFLICT(communication_id,board_id) DO UPDATE SET"
              " rank_limit=excluded.rank_limit, update_mode=excluded.update_mode,"
              " sort_mode=excluded.sort_mode, upload_num_limit=excluded.upload_num_limit,"
              " upload_size_limit=excluded.upload_size_limit");
    q.addBindValue(comId);
    q.addBindValue(boardId);
    q.addBindValue(cfg.rankLimit);
    q.addBindValue(cfg.updateMode);
    q.addBindValue(cfg.sortMode);
    q.addBindValue(cfg.uploadNumLimit);
    q.addBindValue(cfg.uploadSizeLimit);
    if (!q.exec()) {
        qWarning() << "setBoard:" << q.lastError().text();
        return false;
    }
    return true;
}

std::optional<ScoreBoardConfig> ScoreDb::GetBoard(const QString& comId, uint32_t boardId,
                                                  bool createMissing) {
    QSqlQuery q(m_db);
    q.prepare("SELECT rank_limit,update_mode,sort_mode,upload_num_limit,upload_size_limit"
              " FROM score_table WHERE communication_id=? AND board_id=?");
    q.addBindValue(comId);
    q.addBindValue(boardId);
    if (q.exec() && q.next()) {
        ScoreBoardConfig c;
        c.rankLimit = q.value(0).toUInt();
        c.updateMode = q.value(1).toUInt();
        c.sortMode = q.value(2).toUInt();
        c.uploadNumLimit = q.value(3).toUInt();
        c.uploadSizeLimit = q.value(4).toULongLong();
        return c;
    }
    if (createMissing) {
        CreateBoard(comId, boardId);
        return GetBoard(comId, boardId, false);
    }
    return std::nullopt;
}

QVector<std::pair<QPair<QString, uint32_t>, ScoreBoardConfig>> ScoreDb::AllBoards() {
    QVector<std::pair<QPair<QString, uint32_t>, ScoreBoardConfig>> result;
    QSqlQuery q(m_db);
    q.exec("SELECT communication_id,board_id,rank_limit,update_mode,sort_mode,"
           "upload_num_limit,upload_size_limit FROM score_table");
    while (q.next()) {
        ScoreBoardConfig c;
        c.rankLimit = q.value(2).toUInt();
        c.updateMode = q.value(3).toUInt();
        c.sortMode = q.value(4).toUInt();
        c.uploadNumLimit = q.value(5).toUInt();
        c.uploadSizeLimit = q.value(6).toULongLong();
        result.append({{q.value(0).toString(), q.value(1).toUInt()}, c});
    }
    return result;
}

std::optional<ScoreDbError> ScoreDb::RecordScore(const QString& comId, uint32_t boardId,
                                                 const ScoreBoardConfig& cfg,
                                                 const ScoreEntry& entry) {
    // NORMAL_UPDATE: only update when score is better (>= for desc, <= for asc).
    // FORCE_UPDATE:  always overwrite.
    QString sql = "INSERT INTO score(communication_id,board_id,user_id,character_id,"
                  "score,comment,game_info,data_id,timestamp)"
                  " VALUES(?,?,?,?,?,?,?,NULL,?)"
                  " ON CONFLICT(communication_id,board_id,user_id,character_id) DO UPDATE SET"
                  " score=excluded.score, comment=excluded.comment,"
                  " game_info=excluded.game_info, data_id=NULL, timestamp=excluded.timestamp";
    if (cfg.updateMode == 0)
        sql += (cfg.sortMode == 0) ? " WHERE excluded.score >= score"
                                   : " WHERE excluded.score <= score";

    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(comId);
    q.addBindValue(boardId);
    q.addBindValue(static_cast<qlonglong>(entry.userId));
    q.addBindValue(entry.characterId);
    q.addBindValue(static_cast<qlonglong>(entry.score));
    q.addBindValue(entry.comment.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                           : QVariant(entry.comment));
    q.addBindValue(entry.gameInfo.isEmpty() ? QVariant(QMetaType(QMetaType::QByteArray))
                                            : QVariant(entry.gameInfo));
    q.addBindValue(static_cast<qulonglong>(entry.timestamp));

    if (!q.exec()) {
        qCritical() << "recordScore:" << q.lastError().text();
        return ScoreDbError::Internal;
    }
    if (q.numRowsAffected() == 0)
        return ScoreDbError::NotBest;
    return std::nullopt;
}

std::optional<ScoreDbError> ScoreDb::SetScoreDataId(const QString& comId, uint32_t boardId,
                                                    int64_t userId, int32_t characterId,
                                                    int64_t score, uint64_t dataId) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE score SET data_id=?"
              " WHERE communication_id=? AND board_id=? AND user_id=?"
              " AND character_id=? AND score=?");
    q.addBindValue(static_cast<qulonglong>(dataId));
    q.addBindValue(comId);
    q.addBindValue(boardId);
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(characterId);
    q.addBindValue(static_cast<qlonglong>(score));
    if (!q.exec()) {
        qCritical() << "setScoreDataId:" << q.lastError().text();
        return ScoreDbError::Internal;
    }
    if (q.numRowsAffected() == 0)
        return ScoreDbError::Invalid;
    return std::nullopt;
}

QVector<ScoreEntry> ScoreDb::ScoresForBoard(const QString& comId, uint32_t boardId,
                                            const ScoreBoardConfig& cfg) {
    QString order = (cfg.sortMode == 0) ? "s.score DESC, s.timestamp ASC, s.user_id ASC"
                                        : "s.score ASC,  s.timestamp ASC, s.user_id ASC";
    QSqlQuery q(m_db);
    q.prepare("SELECT s.user_id,s.character_id,s.score,s.comment,s.game_info,s.data_id,s.timestamp,"
              "       a.username"
              " FROM score s LEFT JOIN account a ON a.user_id=s.user_id"
              " WHERE s.communication_id=? AND s.board_id=?"
              " ORDER BY " +
              order + " LIMIT " + QString::number(cfg.rankLimit));
    q.addBindValue(comId);
    q.addBindValue(boardId);
    QVector<ScoreEntry> result;
    if (q.exec()) {
        while (q.next()) {
            ScoreEntry e;
            e.userId = q.value(0).toLongLong();
            e.characterId = q.value(1).toInt();
            e.score = q.value(2).toLongLong();
            e.comment = q.value(3).toString();
            e.gameInfo = q.value(4).toByteArray();
            if (!q.value(5).isNull())
                e.dataId = static_cast<uint64_t>(q.value(5).toULongLong());
            e.timestamp = static_cast<uint64_t>(q.value(6).toULongLong());
            e.npid = q.value(7).toString();
            result.append(e);
        }
    }
    return result;
}

QVector<uint64_t> ScoreDb::AllDataIds() {
    QSqlQuery q(m_db);
    q.exec("SELECT data_id FROM score WHERE data_id IS NOT NULL");
    QVector<uint64_t> ids;
    while (q.next())
        ids.append(static_cast<uint64_t>(q.value(0).toULongLong()));
    return ids;
}

std::optional<int64_t> ScoreDb::UserIdForNpid(const QString& npid) {
    QSqlQuery q(m_db);
    q.prepare("SELECT user_id FROM account WHERE username=? COLLATE NOCASE");
    q.addBindValue(npid);
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return std::nullopt;
}
