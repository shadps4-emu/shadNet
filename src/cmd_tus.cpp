// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <vector>
#include <QDebug>
#include <QSqlDatabase>
#include "client_session.h"
#include "proto_utils.h"
#include "score_types.h" // ShadNetTimestamp()
#include "shadnet.pb.h"
#include "tus_db.h"

static constexpr uint32_t TusMaxSelectedFriends = 100;
static constexpr int TusMaxSlotsPerRequest = 64;
static constexpr int TusMaxUsersPerRequest = 101;

static TusDb tusDb(Database* db) {
    return TusDb(db->Conn());
}

// ComId bytes to QString key used for DB lookups.
static QString tusComId(const QByteArray& id) {
    return QString::fromLatin1(id.constData(), id.size());
}

// Resolve a request's target owner. Empty ownerNpId means "self". For write
// operations the resolved owner must be the caller (TUS only lets a user write
// their own slots); returns nullopt to signal Unauthorized/NotFound.
static std::optional<int64_t> resolveTusOwner(TusDb& tdb, const ClientInfo& me,
                                              const std::string& ownerNpId, bool writeOp) {
    if (ownerNpId.empty()) {
        return me.userId;
    }
    auto uid = tdb.UserIdForNpid(QString::fromStdString(ownerNpId));
    if (!uid) {
        return std::nullopt;
    }
    if (writeOp && *uid != me.userId) {
        return std::nullopt;
    }
    return uid;
}

static void fillVariable(TusDb& tdb, shadnet::TusVariable* v, const TusVariableRow& r,
                         const std::string& ownerNpId) {
    v->set_ownernpid(ownerNpId);
    v->set_set(r.set);
    v->set_variable(r.variable);
    v->set_oldvariable(r.oldVariable);
    v->set_lastchangeddate(r.lastChanged);
    v->set_owneraccountid(r.ownerUserId);
    v->set_lastchangedauthoraccountid(r.lastChangedAuthorId);
    // Resolve the author's online id so the client can fill the npid-based
    // struct fields (base/A/CrossSave variants all expose it).
    if (r.set && r.lastChangedAuthorId != 0) {
        if (auto np = tdb.NpidForUserId(r.lastChangedAuthorId)) {
            v->set_lastchangedauthornpid(np->toStdString());
        }
    }
}

static void fillDataStatus(TusDb& tdb, shadnet::TusDataStatus* s, const TusDataRow& r,
                           const std::string& ownerNpId) {
    s->set_ownernpid(ownerNpId);
    s->set_set(r.set);
    s->set_lastchangeddate(r.lastChanged);
    s->set_datasize(r.dataSize);
    s->set_info(r.info.constData(), r.info.size());
    s->set_owneraccountid(r.ownerUserId);
    s->set_lastchangedauthoraccountid(r.lastChangedAuthorId);
    if (r.set && r.lastChangedAuthorId != 0) {
        if (auto np = tdb.NpidForUserId(r.lastChangedAuthorId)) {
            s->set_lastchangedauthornpid(np->toStdString());
        }
    }
}

// TusSetData
//  Request: ComId(12) + TusSetDataRequest blob.  No reply body.
ErrorType ClientSession::CmdTusSetData(StreamExtractor& data) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusSetDataRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);
    const QByteArray blob(req.data().data(), static_cast<int>(req.data().size()));
    const QByteArray info(req.info().data(), static_cast<int>(req.info().size()));

    // Optional conflict guard (sceNpTusSetData* isLastChangedAuthor/Date). Compares
    // against the currently-stored row; an absent row fails any requested check.
    // Base npid-author path resolves the npId to its owner user id (unresolved -> -1,
    // which cannot match a real author so the guard fails safely).
    int64_t guardAuthorId = req.islastchangedauthor();
    if (req.hasauthorcheck() && !req.islastchangedauthornpid().empty()) {
        auto aid = tdb.UserIdForNpid(QString::fromStdString(req.islastchangedauthornpid()));
        guardAuthorId = aid.value_or(-1);
    }
    auto passesGuard = [&](const std::optional<TusDataRow>& cur) -> bool {
        if (!req.hasauthorcheck() && !req.hasdatecheck()) {
            return true;
        }
        if (!cur) {
            return false; // nothing registered -> guarded write does not proceed
        }
        if (req.hasauthorcheck() && cur->lastChangedAuthorId != guardAuthorId) {
            return false;
        }
        if (req.hasdatecheck() && cur->lastChanged > req.islastchangeddate()) {
            return false;
        }
        return true;
    };

    // Targeting precedence: virtualUser (tus_vuser_data) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        const QString vuser = QString::fromStdString(req.virtualuser());
        if (!passesGuard(tdb.GetVUserData(cid, vuser, req.slotid(), /*withPayload=*/false))) {
            return ErrorType::CondFail;
        }
        if (!tdb.SetVUserData(cid, vuser, req.slotid(), blob, info, m_info.userId,
                              ShadNetTimestamp())) {
            return ErrorType::DbFail;
        }
        return ErrorType::NoError;
    }

    int64_t owner = 0;
    if (req.owneraccountid() != 0) {
        // Real-user data writes are self-only (cannot write another account's storage).
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized;
        }
        owner = m_info.userId;
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
    }
    if (!passesGuard(tdb.GetData(cid, owner, req.slotid(), /*withPayload=*/false))) {
        return ErrorType::CondFail;
    }
    if (!tdb.SetData(cid, owner, req.slotid(), blob, info, m_info.userId, ShadNetTimestamp())) {
        return ErrorType::DbFail;
    }
    return ErrorType::NoError;
}

// TusGetData
//  Request: ComId(12) + TusGetDataRequest blob.  Reply: TusGetDataResponse.
ErrorType ClientSession::CmdTusGetData(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetDataRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);

    // Targeting precedence: virtualUser (tus_vuser_data) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        shadnet::TusGetDataResponse resp;
        auto row = tdb.GetVUserData(cid, QString::fromStdString(req.virtualuser()), req.slotid(),
                                    /*withPayload=*/true);
        if (row) {
            fillDataStatus(tdb, resp.mutable_status(), *row, std::string());
            resp.set_data(row->data.constData(), row->data.size());
        } else {
            TusDataRow empty;
            empty.slotId = req.slotid();
            empty.set = false;
            fillDataStatus(tdb, resp.mutable_status(), empty, std::string());
        }
        appendProto(reply, resp);
        return ErrorType::NoError;
    }

    int64_t ownerUserId = 0;
    std::string ownerNpidStr = req.ownernpid();
    if (req.owneraccountid() != 0) {
        ownerUserId = req.owneraccountid();
        auto npidOpt = tdb.NpidForUserId(ownerUserId);
        ownerNpidStr = npidOpt ? npidOpt->toStdString() : std::string();
    } else {
        auto owner = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/false);
        if (!owner) {
            return ErrorType::NotFound;
        }
        ownerUserId = *owner;
    }

    shadnet::TusGetDataResponse resp;
    auto row = tdb.GetData(cid, ownerUserId, req.slotid(), /*withPayload=*/true);
    if (row) {
        fillDataStatus(tdb, resp.mutable_status(), *row, ownerNpidStr);
        resp.set_data(row->data.constData(), row->data.size());
    } else {
        // Unset slot is not an error on hardware: report set=false, no data.
        TusDataRow empty;
        empty.ownerUserId = ownerUserId;
        empty.slotId = req.slotid();
        empty.set = false;
        fillDataStatus(tdb, resp.mutable_status(), empty, ownerNpidStr);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusSetMultiSlotVariable
//  Request: ComId(12) + TusSetMultiSlotVariableRequest.  No reply body.
ErrorType ClientSession::CmdTusSetMultiSlotVariable(StreamExtractor& data) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusSetMultiSlotVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.slotids_size() > TusMaxSlotsPerRequest) {
        qWarning() << "CmdTusSetMultiSlotVariable: slots exceeds cap" << req.slotids_size();
        return ErrorType::Invalid;
    }
    if (req.slotids_size() != req.values_size()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());
    const uint64_t now = ShadNetTimestamp();
    // Targeting precedence: virtualUser (tus_vuser_variable) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        const QString vuser = QString::fromStdString(req.virtualuser());
        auto txn = tdb.BeginWrite();
        if (!txn.Ok()) {
            return ErrorType::DbFail;
        }
        for (int i = 0; i < req.slotids_size(); ++i) {
            if (!tdb.SetVUserVariable(tusComId(comId), vuser, req.slotids(i), req.values(i),
                                      m_info.userId, now)) {
                return ErrorType::DbFail;
            }
        }
        if (!txn.Commit()) {
            return ErrorType::DbFail;
        }
        return ErrorType::NoError;
    }
    int64_t owner = 0;
    if (req.owneraccountid() != 0) {
        // Real-user variable writes are self-only.
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized;
        }
        owner = m_info.userId;
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
    }
    auto txn = tdb.BeginWrite();
    if (!txn.Ok()) {
        return ErrorType::DbFail;
    }
    for (int i = 0; i < req.slotids_size(); ++i) {
        if (!tdb.SetVariable(tusComId(comId), owner, req.slotids(i), req.values(i), m_info.userId,
                             now)) {
            return ErrorType::DbFail;
        }
    }
    if (!txn.Commit()) {
        return ErrorType::DbFail;
    }
    return ErrorType::NoError;
}

// TusGetMultiSlotVariable
//  Request: ComId(12) + TusGetMultiSlotVariableRequest.  Reply: TusVariableResponse.
ErrorType ClientSession::CmdTusGetMultiSlotVariable(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetMultiSlotVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.slotids_size() > TusMaxSlotsPerRequest) {
        qWarning() << "CmdTusGetMultiSlotVariable: slots exceeds cap" << req.slotids_size();
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());

    QVector<int32_t> _slots;
    _slots.reserve(req.slotids_size());
    for (int s : req.slotids()) {
        _slots.append(s);
    }

    // Virtual-user target (VUser variants): separate storage namespace, no account.
    if (!req.virtualuser().empty()) {
        const std::string vu = req.virtualuser();
        auto rows = tdb.GetVUserVariables(tusComId(comId), QString::fromStdString(vu), _slots);
        shadnet::TusVariableResponse resp;
        for (const auto& r : rows) {
            fillVariable(tdb, resp.add_variables(), r, vu);
        }
        appendProto(reply, resp);
        return ErrorType::NoError;
    }

    // Account-id targeting (A-variants) takes precedence: the account id IS the
    // storage owner user_id. Reads may target any user.
    int64_t owner = 0;
    std::string ownerNpId = req.ownernpid();
    if (req.owneraccountid() != 0) {
        owner = req.owneraccountid();
        if (auto np = tdb.NpidForUserId(owner)) {
            ownerNpId = np->toStdString();
        }
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/false);
        if (!resolved) {
            return ErrorType::NotFound;
        }
        owner = *resolved;
    }

    auto rows = tdb.GetVariables(tusComId(comId), owner, _slots);

    shadnet::TusVariableResponse resp;
    for (const auto& r : rows) {
        fillVariable(tdb, resp.add_variables(), r, ownerNpId);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusGetMultiUserVariable
//  Request: ComId(12) + TusGetMultiUserVariableRequest (one slot, many accounts).
//  Reply: TusVariableResponse, one entry per requested account (in order).
// TusTryAndSetVariable
//  Request: ComId(12) + TusTryAndSetVariableRequest.  Reply: TusVariableResponse (1 entry).
ErrorType ClientSession::CmdTusTryAndSetVariable(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusTryAndSetVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.opetype() < 1 || req.opetype() > 6) {
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());

    int64_t owner = 0;
    QString virtualUser;
    std::string ownerLabel;
    if (!req.virtualuser().empty()) {
        virtualUser = QString::fromStdString(req.virtualuser());
        ownerLabel = req.virtualuser();
    } else if (req.owneraccountid() != 0) {
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized; // TUS writes are self-only
        }
        owner = req.owneraccountid();
        ownerLabel = m_info.npid.toStdString();
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
        ownerLabel = req.ownernpid().empty() ? m_info.npid.toStdString() : req.ownernpid();
    }

    const int64_t comparand = req.hascompare() ? req.comparevalue() : req.value();

    TusDb::ConflictCheck check;
    check.hasAuthor = req.hasauthorcheck();
    check.authorId = req.islastchangedauthor();
    if (check.hasAuthor && !req.islastchangedauthornpid().empty()) {
        // Base npid-author path: resolve the npId to its owner user id. An unresolved
        // npId yields -1, which cannot match any real author and so fails the check safely.
        auto aid = tdb.UserIdForNpid(QString::fromStdString(req.islastchangedauthornpid()));
        check.authorId = aid.value_or(-1);
    }
    check.hasDate = req.hasdatecheck();
    check.date = req.islastchangeddate();

    TusDb::AddStatus status = TusDb::AddStatus::Ok;
    auto row = tdb.TryAndSetVariableEx(tusComId(comId), owner, virtualUser, req.slotid(),
                                       req.opetype(), req.value(), comparand, m_info.userId,
                                       ShadNetTimestamp(), check, status);
    if (!row) {
        return status == TusDb::AddStatus::Conflict ? ErrorType::CondFail : ErrorType::DbFail;
    }
    shadnet::TusVariableResponse resp;
    fillVariable(tdb, resp.add_variables(), *row, ownerLabel);
    appendProto(reply, resp);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdTusGetMultiUserVariable(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetMultiUserVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.virtualusers_size() > TusMaxUsersPerRequest ||
        req.ownernpids_size() > TusMaxUsersPerRequest ||
        req.owneraccountids_size() > TusMaxUsersPerRequest) {
        qWarning() << "CmdTusGetMultiUserVariable: users exceeds cap";
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);
    const QVector<int32_t> slot{req.slotid()};

    shadnet::TusVariableResponse resp;
    // Virtual-user array variant takes precedence when present.
    if (req.virtualusers_size() > 0) {
        for (const auto& vu : req.virtualusers()) {
            auto rows = tdb.GetVUserVariables(cid, QString::fromStdString(vu), slot);
            fillVariable(tdb, resp.add_variables(),
                         rows.isEmpty() ? TusVariableRow{} : rows.first(), vu);
        }
        appendProto(reply, resp);
        return ErrorType::NoError;
    }

    // Base npid-array variant: resolve each npId to its owner user id server-side.
    if (req.ownernpids_size() > 0) {
        for (const auto& np : req.ownernpids()) {
            const QString npid = QString::fromStdString(np);
            auto uid = tdb.UserIdForNpid(npid);
            TusVariableRow row{};
            if (uid) {
                auto rows = tdb.GetVariables(cid, *uid, slot);
                row = rows.isEmpty() ? TusVariableRow{} : rows.first();
                row.ownerUserId = *uid;
            }
            fillVariable(tdb, resp.add_variables(), row, np);
        }
        appendProto(reply, resp);
        return ErrorType::NoError;
    }

    for (int64_t accId : req.owneraccountids()) {
        // accId is the storage owner user_id directly.
        auto rows = tdb.GetVariables(cid, accId, slot);
        std::string npid;
        if (auto np = tdb.NpidForUserId(accId)) {
            npid = np->toStdString();
        }
        // GetVariables returns exactly one row per requested slot (set=false if absent).
        TusVariableRow row = rows.isEmpty() ? TusVariableRow{} : rows.first();
        row.ownerUserId = accId; // keep the account id even for unset slots
        fillVariable(tdb, resp.add_variables(), row, npid);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusAddAndGetVariable
//  Request: ComId(12) + TusAddAndGetVariableRequest.  Reply: TusVariableResponse (1 entry).
ErrorType ClientSession::CmdTusAddAndGetVariable(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusAddAndGetVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());

    // Resolve write target. Virtual user wins; then account id (must be self);
    // else npid/self.
    int64_t owner = 0;
    QString virtualUser;
    std::string ownerLabel;
    if (!req.virtualuser().empty()) {
        virtualUser = QString::fromStdString(req.virtualuser());
        ownerLabel = req.virtualuser();
    } else if (req.owneraccountid() != 0) {
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized; // TUS writes are self-only
        }
        owner = req.owneraccountid();
        ownerLabel = m_info.npid.toStdString();
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
        ownerLabel = req.ownernpid().empty() ? m_info.npid.toStdString() : req.ownernpid();
    }

    TusDb::ConflictCheck check;
    check.hasAuthor = req.hasauthorcheck();
    check.authorId = req.islastchangedauthor();
    if (check.hasAuthor && !req.islastchangedauthornpid().empty()) {
        auto aid = tdb.UserIdForNpid(QString::fromStdString(req.islastchangedauthornpid()));
        check.authorId = aid.value_or(-1);
    }
    check.hasDate = req.hasdatecheck();
    check.date = req.islastchangeddate();

    TusDb::AddStatus status = TusDb::AddStatus::Ok;
    auto row =
        tdb.AddAndGetVariableEx(tusComId(comId), owner, virtualUser, req.slotid(), req.invalue(),
                                m_info.userId, ShadNetTimestamp(), check, status);
    if (!row) {
        return status == TusDb::AddStatus::Conflict ? ErrorType::CondFail : ErrorType::DbFail;
    }
    shadnet::TusVariableResponse resp;
    fillVariable(tdb, resp.add_variables(), *row, ownerLabel);
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusGetMultiSlotDataStatus
//  Request: ComId(12) + TusGetMultiSlotDataStatusRequest.  Reply: TusDataStatusResponse.
ErrorType ClientSession::CmdTusGetMultiSlotDataStatus(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetMultiSlotDataStatusRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.slotids_size() > TusMaxSlotsPerRequest) {
        qWarning() << "CmdTusGetMultiSlotDataStatus: slots exceeds cap" << req.slotids_size();
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());
    // Targeting precedence: virtualUser (tus_vuser_data) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        QVector<int32_t> _slots;
        _slots.reserve(req.slotids_size());
        for (int s : req.slotids()) {
            _slots.append(s);
        }
        auto rows = tdb.GetVUserDataStatuses(tusComId(comId),
                                             QString::fromStdString(req.virtualuser()), _slots);
        shadnet::TusDataStatusResponse resp;
        for (const auto& r : rows) {
            fillDataStatus(tdb, resp.add_statuses(), r, std::string());
        }
        appendProto(reply, resp);
        return ErrorType::NoError;
    }
    // Account-variant targets by ownerAccountId (== owner user_id); base targets
    // by npId (empty = self). The response carries the owner npId for base fills.
    int64_t ownerUserId = 0;
    std::string ownerNpidStr = req.ownernpid();
    if (req.owneraccountid() != 0) {
        ownerUserId = req.owneraccountid();
        auto npidOpt = tdb.NpidForUserId(ownerUserId);
        ownerNpidStr = npidOpt ? npidOpt->toStdString() : std::string();
    } else {
        auto owner = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/false);
        if (!owner) {
            return ErrorType::NotFound;
        }
        ownerUserId = *owner;
    }
    QVector<QPair<int64_t, int32_t>> pairs;
    pairs.reserve(req.slotids_size());
    for (int s : req.slotids()) {
        pairs.append({ownerUserId, s});
    }
    auto rows = tdb.GetDataStatuses(tusComId(comId), pairs);

    shadnet::TusDataStatusResponse resp;
    for (const auto& r : rows) {
        fillDataStatus(tdb, resp.add_statuses(), r, ownerNpidStr);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusGetMultiUserDataStatus
//  Request: ComId(12) + TusGetMultiUserDataStatusRequest (one slot, many owners).
//  Reply: TusDataStatusResponse, one entry per requested owner (in order).
ErrorType ClientSession::CmdTusGetMultiUserDataStatus(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetMultiUserDataStatusRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.virtualusers_size() > TusMaxUsersPerRequest ||
        req.ownernpids_size() > TusMaxUsersPerRequest ||
        req.owneraccountids_size() > TusMaxUsersPerRequest) {
        qWarning() << "CmdTusGetMultiUserDataStatus: users exceeds cap";
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);

    shadnet::TusDataStatusResponse resp;
    // VUser variant: one row per virtual user (tus_vuser_data), empty owner npId.
    for (const auto& vu : req.virtualusers()) {
        auto row = tdb.GetVUserData(cid, QString::fromStdString(vu), req.slotid(),
                                    /*withPayload=*/false);
        if (row) {
            fillDataStatus(tdb, resp.add_statuses(), *row, std::string());
        } else {
            TusDataRow empty;
            empty.slotId = req.slotid();
            empty.set = false;
            fillDataStatus(tdb, resp.add_statuses(), empty, std::string());
        }
    }
    for (const auto& npid : req.ownernpids()) {
        auto uid = tdb.UserIdForNpid(QString::fromStdString(npid));
        if (!uid) {
            TusDataRow miss;
            miss.slotId = req.slotid();
            miss.set = false;
            fillDataStatus(tdb, resp.add_statuses(), miss, npid);
            continue;
        }
        auto row = tdb.GetData(cid, *uid, req.slotid(), /*withPayload=*/false);
        if (row) {
            fillDataStatus(tdb, resp.add_statuses(), *row, npid);
        } else {
            TusDataRow empty;
            empty.ownerUserId = *uid;
            empty.slotId = req.slotid();
            empty.set = false;
            fillDataStatus(tdb, resp.add_statuses(), empty, npid);
        }
    }
    for (int64_t accId : req.owneraccountids()) {
        // accId is the storage owner user_id directly.
        std::string accNpid;
        if (auto np = tdb.NpidForUserId(accId)) {
            accNpid = np->toStdString();
        }
        auto row = tdb.GetData(cid, accId, req.slotid(), /*withPayload=*/false);
        if (row) {
            fillDataStatus(tdb, resp.add_statuses(), *row, accNpid);
        } else {
            TusDataRow empty2;
            empty2.ownerUserId = accId;
            empty2.slotId = req.slotid();
            empty2.set = false;
            fillDataStatus(tdb, resp.add_statuses(), empty2, accNpid);
        }
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusGetFriendsDataStatus
//  Request: ComId(12) + TusGetFriendsDataStatusRequest.  Reply: TusDataStatusResponse.
ErrorType ClientSession::CmdTusGetFriendsDataStatus(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetFriendsDataStatusRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);

    // (userId, npid) targets: optionally self first, then mutual friends.
    QVector<QPair<int64_t, QString>> targets;
    if (req.includeself()) {
        targets.append({m_info.userId, m_info.npid});
    }
    for (const auto& f : m_db->GetRelationships(m_info.userId).friends) {
        targets.append({f.first, f.second});
    }

    // Fetch each target's status row; only users that actually have data in this
    // slot count as a "registered" status (the call returns the count of these).
    struct FriendStatus {
        TusDataRow row;
        std::string npid;
    };
    std::vector<FriendStatus> rows;
    for (const auto& t : targets) {
        auto row = tdb.GetData(cid, t.first, req.slotid(), /*withPayload=*/false);
        if (row && row->set) {
            rows.push_back({*row, t.second.toStdString()});
        }
    }

    // 1 = descending date, 2 = ascending date.
    const bool ascending = req.sorttype() == 2;
    std::sort(rows.begin(), rows.end(), [ascending](const FriendStatus& a, const FriendStatus& b) {
        if (a.row.lastChanged != b.row.lastChanged) {
            return ascending ? a.row.lastChanged < b.row.lastChanged
                             : a.row.lastChanged > b.row.lastChanged;
        }
        return a.row.ownerUserId < b.row.ownerUserId;
    });

    // hits: total friends with a value registered in this slot, BEFORE the
    // start offset and cap are applied (OrbisNpTusGetFriendsVariableOptParam::hits).
    const uint32_t hits = static_cast<uint32_t>(rows.size());

    // startOffset lets a title page past the first 100 ranked friends.
    const uint32_t start = req.startoffset();
    if (start >= rows.size()) {
        rows.clear();
    } else if (start > 0) {
        rows.erase(rows.begin(), rows.begin() + start);
    }

    const uint32_t cap =
        req.max() > 0 ? std::min(req.max(), TusMaxSelectedFriends) : TusMaxSelectedFriends;
    if (rows.size() > cap) {
        rows.resize(cap);
    }

    shadnet::TusDataStatusResponse resp;
    resp.set_total(hits);
    for (const auto& fs : rows) {
        fillDataStatus(tdb, resp.add_statuses(), fs.row, fs.npid);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdTusGetFriendsVariable(StreamExtractor& data, QByteArray& reply) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusGetFriendsVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    auto tdb = tusDb(m_db.get());
    const QString cid = tusComId(comId);

    // (userId, npid) targets: optionally self first, then mutual friends.
    QVector<QPair<int64_t, QString>> targets;
    if (req.includeself()) {
        targets.append({m_info.userId, m_info.npid});
    }
    for (const auto& f : m_db->GetRelationships(m_info.userId).friends) {
        targets.append({f.first, f.second});
    }

    // Only friends that actually have the variable set in this slot are returned;
    // the count of these is the call's return value.
    struct FriendVar {
        TusVariableRow row;
        std::string npid;
    };
    std::vector<FriendVar> rows;
    const QVector<int32_t> slot{req.slotid()};
    for (const auto& t : targets) {
        auto vars = tdb.GetVariables(cid, t.first, slot);
        if (!vars.isEmpty() && vars.first().set) {
            TusVariableRow row = vars.first();
            row.ownerUserId = t.first;
            rows.push_back({row, t.second.toStdString()});
        }
    }

    // 1 = DESC date, 2 = ASC date, 3 = DESC value, 4 = ASC value.
    const int32_t st = req.sorttype();
    std::sort(rows.begin(), rows.end(), [st](const FriendVar& a, const FriendVar& b) {
        switch (st) {
        case 2:
            if (a.row.lastChanged != b.row.lastChanged)
                return a.row.lastChanged < b.row.lastChanged;
            break;
        case 3:
            if (a.row.variable != b.row.variable)
                return a.row.variable > b.row.variable;
            break;
        case 4:
            if (a.row.variable != b.row.variable)
                return a.row.variable < b.row.variable;
            break;
        case 1:
        default:
            if (a.row.lastChanged != b.row.lastChanged)
                return a.row.lastChanged > b.row.lastChanged;
            break;
        }
        return a.row.ownerUserId < b.row.ownerUserId;
    });

    // hits: total friends with a value registered in this slot, BEFORE the
    // start offset and cap are applied (OrbisNpTusGetFriendsVariableOptParam::hits).
    const uint32_t hits = static_cast<uint32_t>(rows.size());

    // startOffset lets a title page past the first 100 ranked friends.
    const uint32_t start = req.startoffset();
    if (start >= rows.size()) {
        rows.clear();
    } else if (start > 0) {
        rows.erase(rows.begin(), rows.begin() + start);
    }

    const uint32_t cap =
        req.max() > 0 ? std::min(req.max(), TusMaxSelectedFriends) : TusMaxSelectedFriends;
    if (rows.size() > cap) {
        rows.resize(cap);
    }

    shadnet::TusVariableResponse resp;
    resp.set_total(hits);
    for (const auto& fv : rows) {
        fillVariable(tdb, resp.add_variables(), fv.row, fv.npid);
    }
    appendProto(reply, resp);
    return ErrorType::NoError;
}

// TusDeleteMultiSlotData
//  Request: ComId(12) + TusDeleteMultiSlotDataRequest.  No reply body.
ErrorType ClientSession::CmdTusDeleteMultiSlotData(StreamExtractor& data) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusDeleteMultiSlotDataRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.slotids_size() > TusMaxSlotsPerRequest) {
        qWarning() << "CmdTusDeleteMultiSlotData: slots exceeds cap" << req.slotids_size();
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());
    QVector<int32_t> _slots;
    _slots.reserve(req.slotids_size());
    for (int s : req.slotids()) {
        _slots.append(s);
    }
    // Targeting precedence: virtualUser (vuser tables) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        if (!tdb.DeleteVUserSlots(tusComId(comId), QString::fromStdString(req.virtualuser()),
                                  _slots)) {
            return ErrorType::DbFail;
        }
        return ErrorType::NoError;
    }
    int64_t owner = 0;
    if (req.owneraccountid() != 0) {
        // Real-user deletes are self-only.
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized;
        }
        owner = m_info.userId;
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
    }
    if (!tdb.DeleteSlots(tusComId(comId), owner, _slots)) {
        return ErrorType::DbFail;
    }
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdTusDeleteMultiSlotVariable(StreamExtractor& data) {
    QByteArray comId = data.getBytes(12);
    shadnet::TusDeleteMultiSlotVariableRequest req;
    if (!decodeProto(req, data) || data.error()) {
        return ErrorType::Malformed;
    }
    if (req.slotids_size() > TusMaxSlotsPerRequest) {
        qWarning() << "CmdTusDeleteMultiSlotVariable: slots exceeds cap" << req.slotids_size();
        return ErrorType::Invalid;
    }
    auto tdb = tusDb(m_db.get());
    QVector<int32_t> _slots;
    _slots.reserve(req.slotids_size());
    for (int s : req.slotids()) {
        _slots.append(s);
    }
    // Targeting precedence: virtualUser (tus_vuser_variable) > ownerAccountId > ownerNpId/self.
    if (!req.virtualuser().empty()) {
        if (!tdb.DeleteVUserVariableSlots(tusComId(comId),
                                          QString::fromStdString(req.virtualuser()), _slots)) {
            return ErrorType::DbFail;
        }
        return ErrorType::NoError;
    }
    int64_t owner = 0;
    if (req.owneraccountid() != 0) {
        // Real-user variable deletes are self-only.
        if (req.owneraccountid() != m_info.userId) {
            return ErrorType::Unauthorized;
        }
        owner = m_info.userId;
    } else {
        auto resolved = resolveTusOwner(tdb, m_info, req.ownernpid(), /*writeOp=*/true);
        if (!resolved) {
            return ErrorType::Unauthorized;
        }
        owner = *resolved;
    }
    if (!tdb.DeleteVariableSlots(tusComId(comId), owner, _slots)) {
        return ErrorType::DbFail;
    }
    return ErrorType::NoError;
}
