// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>

#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"
#include "trophy_time.h"

QString comIdStr(const QByteArray& id) {
    QByteArray trimmed = id;
    const int nul = trimmed.indexOf('\0');
    if (nul >= 0)
        trimmed.truncate(nul);
    return QString::fromLatin1(trimmed);
}

constexpr int MaxTrophiesPerSync = 1024;
constexpr int MaxTrophyId = 4096;

bool ValidTrophyId(int32_t id) {
    return id >= 0 && id < MaxTrophyId;
}

ErrorType ClientSession::CmdUnlockTrophy(StreamExtractor& data) {
    const QByteArray comIdRaw = data.getBytes(12);
    shadnet::UnlockTrophyRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    if (!TrophiesEnabled())
        return ErrorType::Unsupported;

    const QString comId = comIdStr(comIdRaw);
    if (comId.isEmpty()) {
        qWarning() << "UnlockTrophy: empty com id from" << m_info.npid;
        return ErrorType::InvalidInput;
    }
    if (!ValidTrophyId(req.trophyid())) {
        qWarning() << "UnlockTrophy: rejecting trophy id" << req.trophyid() << "from"
                   << m_info.npid;
        return ErrorType::InvalidInput;
    }
    const int64_t earnedAt = TrophyUnixSeconds(req.timestamp(), QDateTime::currentSecsSinceEpoch());

    if (!m_db->RecordUserTrophy(m_info.userId, comId, req.trophyid(), earnedAt)) {
        qCritical() << "UnlockTrophy: DB error for" << m_info.npid;
        return ErrorType::DbFail;
    }

    qInfo().nospace().noquote() << "UnlockTrophy: " << m_info.npid << " unlocked " << req.trophyid()
                                << " in " << comId;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSyncTrophies(StreamExtractor& data, QByteArray& reply) {
    const QByteArray comIdRaw = data.getBytes(12);
    shadnet::SyncTrophiesRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    if (!TrophiesEnabled())
        return ErrorType::Unsupported;

    const QString comId = comIdStr(comIdRaw);
    if (comId.isEmpty()) {
        qWarning() << "SyncTrophies: empty com id from" << m_info.npid;
        return ErrorType::InvalidInput;
    }
    if (req.trophies_size() > MaxTrophiesPerSync) {
        qWarning() << "SyncTrophies:" << req.trophies_size() << "entries from" << m_info.npid
                   << "exceeds the" << MaxTrophiesPerSync << "cap";
        return ErrorType::InvalidInput;
    }

    QList<QPair<int32_t, int64_t>> uploaded;
    uploaded.reserve(req.trophies_size());
    for (const auto& t : req.trophies()) {
        if (!ValidTrophyId(t.trophyid())) {
            qWarning() << "SyncTrophies: rejecting trophy id" << t.trophyid() << "from"
                       << m_info.npid;
            return ErrorType::InvalidInput;
        }
        uploaded.append(
            {t.trophyid(), TrophyUnixSeconds(t.timestamp(), QDateTime::currentSecsSinceEpoch())});
    }
    if (!m_db->RecordUserTrophies(m_info.userId, comId, uploaded)) {
        qCritical() << "SyncTrophies: DB error storing" << uploaded.size() << "for" << m_info.npid;
        return ErrorType::DbFail;
    }

    const auto merged = m_db->ListUserTrophiesForGame(m_info.userId, comId);
    shadnet::SyncTrophiesReply rep;
    for (const auto& t : merged) {
        auto* entry = rep.add_trophies();
        entry->set_trophyid(t.trophyId);
        entry->set_timestamp(static_cast<uint64_t>(t.earnedAt));
    }
    appendProto(reply, rep);

    qInfo().nospace().noquote() << "SyncTrophies: " << m_info.npid << " uploaded "
                                << uploaded.size() << " for " << comId << ", holds "
                                << merged.size() << " after merge";
    return ErrorType::NoError;
}

bool ClientSession::TrophiesEnabled() const {
    return m_shared && m_shared->config && m_shared->config->IsTrophiesEnabled();
}
