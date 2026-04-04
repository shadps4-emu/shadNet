#include <QDebug>
#include <QSqlDatabase>
#include "client_session.h"
#include "proto_utils.h"
#include "score_db.h"

// Helper: open a ScoreDb on this session's connection.
static ScoreDb scoreDb(Database* db) {
    return ScoreDb(db->Conn());
}

// ComId bytes to QString key used for cache and DB lookups.
static QString comIdStr(const QByteArray& id) {
    return QString::fromLatin1(id.constData(), id.size());
}

ErrorType ClientSession::CmdGetBoardInfos(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRecordScore(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRecordScoreData(StreamExtractor& data) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetScoreData(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetScoreRange(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetScoreFriends(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetScoreNpid(StreamExtractor& data, QByteArray& reply) {
    // todo
    return ErrorType::NoError;
}
