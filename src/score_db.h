#pragma once
#include <optional>
#include <QSqlDatabase>
#include <QVector>
#include "score_types.h"

enum class ScoreDbError { Internal, NotBest, Invalid };

class ScoreDb {
public:
    explicit ScoreDb(const QSqlDatabase& db) : m_db(db) {}

    // Score operations
    std::optional<ScoreDbError> RecordScore(const QString& comId, uint32_t boardId,
                                            const ScoreBoardConfig& cfg, const ScoreEntry& entry);

private:
    QSqlDatabase m_db;
};
