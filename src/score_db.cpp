#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include "score_db.h"

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
