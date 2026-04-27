// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QSqlDatabase>
#include "client_session.h"
#include "proto_utils.h"
#include "score_db.h"
#include "shadnet.pb.h"

// Wrap this session's DB connection for score operations.
static ScoreDb scoreDb(Database* db) {
    return ScoreDb(db->Conn());
}

// ComId bytes to QString key used for cache and DB lookups.
static QString comIdStr(const QByteArray& id) {
    return QString::fromLatin1(id.constData(), id.size());
}
// GetBoardInfos
// Request:  ComId(12) + boardId(u32 LE)
// Reply:    u32 LE size + BoardInfo protobuf blob
ErrorType ClientSession::CmdGetBoardInfos(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    uint32_t boardId = data.get<uint32_t>();
    if (data.error())
        return ErrorType::Malformed;

    auto sdb = scoreDb(m_db.get());
    auto cfg = sdb.GetBoard(comIdStr(comId), boardId, false);
    if (!cfg)
        return ErrorType::NotFound;

    shadnet::BoardInfo bi;
    bi.set_ranklimit(cfg->rankLimit);
    bi.set_updatemode(cfg->updateMode);
    bi.set_sortmode(cfg->sortMode);
    bi.set_uploadnumlimit(cfg->uploadNumLimit);
    bi.set_uploadsizelimit(cfg->uploadSizeLimit);
    appendProto(reply, bi);
    return ErrorType::NoError;
}

// RecordScore
//  Request:  ComId(12) + RecordScoreRequest blob
//  Reply:    rank (u32 LE)
ErrorType ClientSession::CmdRecordScore(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::RecordScoreRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString cid = comIdStr(comId);
    auto sdb = scoreDb(m_db.get());
    auto cfg = sdb.GetBoard(cid, req.boardid(), true);
    if (!cfg)
        return ErrorType::DbFail;

    ScoreEntry entry;
    entry.userId = m_info.userId;
    entry.characterId = req.pcid();
    entry.score = req.score();
    entry.comment =
        QString::fromUtf8(req.comment().c_str(), static_cast<int>(req.comment().size()));
    entry.gameInfo = QByteArray(req.data().data(), static_cast<int>(req.data().size()));
    entry.timestamp = ShadNetTimestamp();
    entry.npid = m_info.npid;

    auto dbErr = sdb.RecordScore(cid, req.boardid(), *cfg, entry);
    if (dbErr) {
        if (*dbErr == ScoreDbError::NotBest)
            return ErrorType::ScoreNotBest;
        return ErrorType::DbFail;
    }

    uint32_t rank = m_shared->scoreCache->InsertScore(cid, req.boardid(), *cfg, entry);
    appendU32LE(reply, rank);
    qInfo() << "RecordScore:" << m_info.npid << "board" << req.boardid() << "rank" << rank;
    return ErrorType::NoError;
}

// RecordScoreData
// Request:  ComId(12) + RecordScoreGameDataRequest blob + raw data blob
// Reply:    error byte only
ErrorType ClientSession::CmdRecordScoreData(StreamExtractor& data) {
    QByteArray comId = data.getBytes(12);
    shadnet::RecordScoreGameDataRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;
    QByteArray rawData = data.getRawData();
    if (data.error())
        return ErrorType::Malformed;

    QString cid = comIdStr(comId);

    auto cacheErr = m_shared->scoreCache->ContainsScoreWithNoData(cid, req.boardid(), m_info.userId,
                                                                  req.pcid(), req.score());
    if (cacheErr) {
        switch (*cacheErr) {
        case ScoreCacheError::NotFound:
            return ErrorType::NotFound;
        case ScoreCacheError::Invalid:
            return ErrorType::ScoreInvalid;
        case ScoreCacheError::HasData:
            return ErrorType::ScoreHasData;
        }
    }

    uint64_t fileId = m_shared->scoreFiles->Create(rawData);
    if (fileId == 0)
        return ErrorType::DbFail;

    if (m_shared->scoreCache->SetGameData(cid, req.boardid(), m_info.userId, req.pcid(), fileId)) {
        m_shared->scoreFiles->Remove(fileId);
        return ErrorType::NotFound;
    }

    auto sdb = scoreDb(m_db.get());
    sdb.SetScoreDataId(cid, req.boardid(), m_info.userId, req.pcid(), req.score(), fileId);
    return ErrorType::NoError;
}

// GetScoreData
// Request:  ComId(12) + GetScoreGameDataRequest blob
// Reply:    u32 LE size + raw data bytes
ErrorType ClientSession::CmdGetScoreData(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreGameDataRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;
    if (req.npid().empty())
        return ErrorType::Malformed;

    QString cid = comIdStr(comId);
    auto sdb = scoreDb(m_db.get());
    auto uidOpt = sdb.UserIdForNpid(
        QString::fromUtf8(req.npid().c_str(), static_cast<int>(req.npid().size())));
    if (!uidOpt)
        return ErrorType::NotFound;

    auto [ok, fileId] =
        m_shared->scoreCache->GetGameDataId(cid, req.boardid(), *uidOpt, req.pcid());
    if (!ok)
        return ErrorType::NotFound;

    auto fileData = m_shared->scoreFiles->Read(fileId);
    if (!fileData)
        return ErrorType::NotFound;

    appendBlob(reply, *fileData);
    return ErrorType::NoError;
}

// GetScoreRange
// Request:  ComId(12) + GetScoreRangeRequest blob
// Reply:    u32 LE size + GetScoreResponse protobuf blob
ErrorType ClientSession::CmdGetScoreRange(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreRangeRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    auto resp =
        m_shared->scoreCache->GetScoreRange(comIdStr(comId), req.boardid(), req.startrank(),
                                            req.numranks(), req.withcomment(), req.withgameinfo());
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// GetScoreFriends
// Request:  ComId(12) + GetScoreFriendsRequest blob
// Reply:    u32 LE size + GetScoreResponse protobuf blob
ErrorType ClientSession::CmdGetScoreFriends(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreFriendsRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QVector<QPair<int64_t, int32_t>> ids;
    if (req.includeself())
        ids.append({m_info.userId, 0});

    {
        QReadLocker lk(&m_shared->clientsLock);
        auto self = m_shared->clients.find(m_info.userId);
        if (self != m_shared->clients.end()) {
            for (auto it = self->friends.begin(); it != self->friends.end(); ++it) {
                if (req.max() > 0 && static_cast<uint32_t>(ids.size()) >= req.max())
                    break;
                ids.append({it.key(), 0});
            }
        }
    }

    auto resp = m_shared->scoreCache->GetScoreByIds(comIdStr(comId), req.boardid(), ids,
                                                    req.withcomment(), req.withgameinfo());
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// GetScoreNpid
// Request:  ComId(12) + GetScoreNpIdRequest blob
// Reply:    u32 LE size + GetScoreResponse protobuf blob
ErrorType ClientSession::CmdGetScoreNpid(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreNpIdRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString cid = comIdStr(comId);
    auto sdb = scoreDb(m_db.get());

    QVector<QPair<int64_t, int32_t>> ids;
    ids.reserve(req.npids_size());
    for (const auto& e : req.npids()) {
        auto uid = sdb.UserIdForNpid(
            QString::fromUtf8(e.npid().c_str(), static_cast<int>(e.npid().size())));
        ids.append({uid.value_or(0), e.pcid()});
    }

    auto resp = m_shared->scoreCache->GetScoreByIds(cid, req.boardid(), ids, req.withcomment(),
                                                    req.withgameinfo());
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// GetScoreAccountId
// Request:  ComId(12) + GetScoreAccountIdRequest blob
// Reply:    u32 LE size + GetScoreResponse protobuf blob
ErrorType ClientSession::CmdGetScoreAccountId(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreAccountIdRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QVector<QPair<int64_t, int32_t>> ids;
    ids.reserve(req.ids_size());
    for (const auto& e : req.ids()) {
        ids.append({e.accountid(), e.pcid()});
    }

    auto resp = m_shared->scoreCache->GetScoreByIds(comIdStr(comId), req.boardid(), ids,
                                                    req.withcomment(), req.withgameinfo());
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// GetScoreGameDataByAccId
// Request:  ComId(12) + GetScoreGameDataByAccountIdRequest blob
// Reply:    u32 LE size + raw game-data bytes
ErrorType ClientSession::CmdGetScoreGameDataByAccId(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::GetScoreGameDataByAccountIdRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;
    if (req.accountid() == 0)
        return ErrorType::Malformed;

    auto [ok, fileId] = m_shared->scoreCache->GetGameDataId(comIdStr(comId), req.boardid(),
                                                            req.accountid(), req.pcid());
    if (!ok)
        return ErrorType::NotFound;

    auto fileData = m_shared->scoreFiles->Read(fileId);
    if (!fileData)
        return ErrorType::NotFound;

    appendBlob(reply, *fileData);
    return ErrorType::NoError;
}