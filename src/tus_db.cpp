// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QSqlQuery>
#include <QVariant>
#include "tus_db.h"

bool TusDb::SetVariable(const QString& comId, int64_t owner, int32_t slot, int64_t value,
                        int64_t authorId, uint64_t now) {
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tus_variable(communication_id,owner_user_id,slot_id,variable,"
              "last_changed,last_changed_author_id) VALUES(?,?,?,?,?,?) "
              "ON CONFLICT(communication_id,owner_user_id,slot_id) DO UPDATE SET "
              "variable=excluded.variable,last_changed=excluded.last_changed,"
              "last_changed_author_id=excluded.last_changed_author_id");
    q.addBindValue(comId);
    q.addBindValue(static_cast<qint64>(owner));
    q.addBindValue(slot);
    q.addBindValue(static_cast<qint64>(value));
    q.addBindValue(static_cast<qint64>(now));
    q.addBindValue(static_cast<qint64>(authorId));
    return q.exec();
}

bool TusDb::SetVUserVariable(const QString& comId, const QString& virtualUser, int32_t slot,
                             int64_t value, int64_t authorId, uint64_t now) {
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tus_vuser_variable(communication_id,virtual_user,slot_id,variable,"
              "last_changed,last_changed_author_id) VALUES(?,?,?,?,?,?) "
              "ON CONFLICT(communication_id,virtual_user,slot_id) DO UPDATE SET "
              "variable=excluded.variable,last_changed=excluded.last_changed,"
              "last_changed_author_id=excluded.last_changed_author_id");
    q.addBindValue(comId);
    q.addBindValue(virtualUser);
    q.addBindValue(slot);
    q.addBindValue(static_cast<qint64>(value));
    q.addBindValue(static_cast<qint64>(now));
    q.addBindValue(static_cast<qint64>(authorId));
    return q.exec();
}

// Read-modify-write transactions (AddAndGet / TryAndSet) must take the write
// lock up front.
static bool beginWriteTxn(QSqlDatabase& db) {
    QSqlQuery q(db);
    return q.exec(QStringLiteral("BEGIN IMMEDIATE"));
}

TusDb::WriteTxn::WriteTxn(QSqlDatabase db) : m_db(db) {
    m_active = beginWriteTxn(m_db);
}

bool TusDb::WriteTxn::Commit() {
    if (!m_active || m_done) {
        return false;
    }
    m_done = true;
    return m_db.commit();
}

TusDb::WriteTxn::~WriteTxn() {
    if (m_active && !m_done) {
        m_db.rollback();
    }
}

std::optional<TusVariableRow> TusDb::AddAndGetVariable(const QString& comId, int64_t owner,
                                                       int32_t slot, int64_t delta,
                                                       int64_t authorId, uint64_t now) {
    beginWriteTxn(m_db);

    QSqlQuery sel(m_db);
    sel.prepare("SELECT variable FROM tus_variable WHERE communication_id=? AND owner_user_id=? "
                "AND slot_id=?");
    sel.addBindValue(comId);
    sel.addBindValue(static_cast<qint64>(owner));
    sel.addBindValue(slot);
    int64_t oldVal = 0;
    bool existed = false;
    if (sel.exec() && sel.next()) {
        oldVal = sel.value(0).toLongLong();
        existed = true;
    }
    const int64_t newVal = oldVal + delta;

    QSqlQuery up(m_db);
    up.prepare("INSERT INTO tus_variable(communication_id,owner_user_id,slot_id,variable,"
               "last_changed,last_changed_author_id) VALUES(?,?,?,?,?,?) "
               "ON CONFLICT(communication_id,owner_user_id,slot_id) DO UPDATE SET "
               "variable=excluded.variable,last_changed=excluded.last_changed,"
               "last_changed_author_id=excluded.last_changed_author_id");
    up.addBindValue(comId);
    up.addBindValue(static_cast<qint64>(owner));
    up.addBindValue(slot);
    up.addBindValue(static_cast<qint64>(newVal));
    up.addBindValue(static_cast<qint64>(now));
    up.addBindValue(static_cast<qint64>(authorId));
    if (!up.exec()) {
        m_db.rollback();
        return std::nullopt;
    }
    m_db.commit();

    TusVariableRow r;
    r.ownerUserId = owner;
    r.slotId = slot;
    r.set = true;
    r.variable = newVal;
    r.oldVariable = existed ? oldVal : 0;
    r.lastChanged = now;
    r.lastChangedAuthorId = authorId;
    return r;
}

std::optional<TusVariableRow> TusDb::AddAndGetVariableEx(const QString& comId, int64_t owner,
                                                         const QString& virtualUser, int32_t slot,
                                                         int64_t delta, int64_t authorId,
                                                         uint64_t now, const ConflictCheck& check,
                                                         AddStatus& status) {
    const bool vuser = !virtualUser.isEmpty();
    const QString table =
        vuser ? QStringLiteral("tus_vuser_variable") : QStringLiteral("tus_variable");
    const QString keyCol = vuser ? QStringLiteral("virtual_user") : QStringLiteral("owner_user_id");

    beginWriteTxn(m_db);

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT variable,last_changed,last_changed_author_id FROM %1 "
                               "WHERE communication_id=? AND %2=? AND slot_id=?")
                    .arg(table, keyCol));
    sel.addBindValue(comId);
    if (vuser) {
        sel.addBindValue(virtualUser);
    } else {
        sel.addBindValue(static_cast<qint64>(owner));
    }
    sel.addBindValue(slot);
    int64_t oldVal = 0;
    uint64_t curDate = 0;
    int64_t curAuthor = 0;
    bool existed = false;
    if (sel.exec() && sel.next()) {
        oldVal = sel.value(0).toLongLong();
        curDate = sel.value(1).toULongLong();
        curAuthor = sel.value(2).toLongLong();
        existed = true;
    }

    if (check.hasAuthor || check.hasDate) {
        if (!existed || (check.hasAuthor && curAuthor != check.authorId) ||
            (check.hasDate && curDate > check.date)) {
            m_db.rollback();
            status = AddStatus::Conflict;
            return std::nullopt;
        }
    }

    const int64_t newVal = oldVal + delta;

    QSqlQuery up(m_db);
    up.prepare(QStringLiteral("INSERT INTO %1(communication_id,%2,slot_id,variable,last_changed,"
                              "last_changed_author_id) VALUES(?,?,?,?,?,?) "
                              "ON CONFLICT(communication_id,%2,slot_id) DO UPDATE SET "
                              "variable=excluded.variable,last_changed=excluded.last_changed,"
                              "last_changed_author_id=excluded.last_changed_author_id")
                   .arg(table, keyCol));
    up.addBindValue(comId);
    if (vuser) {
        up.addBindValue(virtualUser);
    } else {
        up.addBindValue(static_cast<qint64>(owner));
    }
    up.addBindValue(slot);
    up.addBindValue(static_cast<qint64>(newVal));
    up.addBindValue(static_cast<qint64>(now));
    up.addBindValue(static_cast<qint64>(authorId));
    if (!up.exec()) {
        m_db.rollback();
        status = AddStatus::DbError;
        return std::nullopt;
    }
    m_db.commit();

    TusVariableRow r;
    r.ownerUserId = vuser ? 0 : owner;
    r.slotId = slot;
    r.set = true;
    r.variable = newVal;
    r.oldVariable = existed ? oldVal : 0;
    r.lastChanged = now;
    r.lastChangedAuthorId = authorId;
    status = AddStatus::Ok;
    return r;
}

std::optional<TusVariableRow> TusDb::TryAndSetVariableEx(const QString& comId, int64_t owner,
                                                         const QString& virtualUser, int32_t slot,
                                                         int32_t opeType, int64_t value,
                                                         int64_t comparand, int64_t authorId,
                                                         uint64_t now, const ConflictCheck& check,
                                                         AddStatus& status) {
    const bool vuser = !virtualUser.isEmpty();
    const QString table =
        vuser ? QStringLiteral("tus_vuser_variable") : QStringLiteral("tus_variable");
    const QString keyCol = vuser ? QStringLiteral("virtual_user") : QStringLiteral("owner_user_id");

    beginWriteTxn(m_db);

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT variable,last_changed,last_changed_author_id FROM %1 "
                               "WHERE communication_id=? AND %2=? AND slot_id=?")
                    .arg(table, keyCol));
    sel.addBindValue(comId);
    if (vuser) {
        sel.addBindValue(virtualUser);
    } else {
        sel.addBindValue(static_cast<qint64>(owner));
    }
    sel.addBindValue(slot);
    int64_t curVal = 0;
    uint64_t curDate = 0;
    int64_t curAuthor = 0;
    bool existed = false;
    if (sel.exec() && sel.next()) {
        curVal = sel.value(0).toLongLong();
        curDate = sel.value(1).toULongLong();
        curAuthor = sel.value(2).toLongLong();
        existed = true;
    }

    // Author/date mutual-exclusion guard (fails outright if nothing registered).
    if (check.hasAuthor || check.hasDate) {
        if (!existed || (check.hasAuthor && curAuthor != check.authorId) ||
            (check.hasDate && curDate > check.date)) {
            m_db.rollback();
            status = AddStatus::Conflict;
            return std::nullopt;
        }
    }

    // Evaluate the write condition. An unset slot is treated as "condition met".
    bool conditionMet = true;
    if (existed) {
        switch (opeType) {
        case 1: // EQUAL
            conditionMet = comparand == curVal;
            break;
        case 2: // NOT_EQUAL
            conditionMet = comparand != curVal;
            break;
        case 3: // GREATER_THAN
            conditionMet = comparand > curVal;
            break;
        case 4: // GREATER_OR_EQUAL
            conditionMet = comparand >= curVal;
            break;
        case 5: // LESS_THAN
            conditionMet = comparand < curVal;
            break;
        case 6: // LESS_OR_EQUAL
            conditionMet = comparand <= curVal;
            break;
        default:
            m_db.rollback();
            status = AddStatus::DbError;
            return std::nullopt;
        }
    }

    int64_t finalVal = curVal;
    if (conditionMet) {
        finalVal = value;
        QSqlQuery up(m_db);
        up.prepare(
            QStringLiteral("INSERT INTO %1(communication_id,%2,slot_id,variable,last_changed,"
                           "last_changed_author_id) VALUES(?,?,?,?,?,?) "
                           "ON CONFLICT(communication_id,%2,slot_id) DO UPDATE SET "
                           "variable=excluded.variable,last_changed=excluded.last_changed,"
                           "last_changed_author_id=excluded.last_changed_author_id")
                .arg(table, keyCol));
        up.addBindValue(comId);
        if (vuser) {
            up.addBindValue(virtualUser);
        } else {
            up.addBindValue(static_cast<qint64>(owner));
        }
        up.addBindValue(slot);
        up.addBindValue(static_cast<qint64>(finalVal));
        up.addBindValue(static_cast<qint64>(now));
        up.addBindValue(static_cast<qint64>(authorId));
        if (!up.exec()) {
            m_db.rollback();
            status = AddStatus::DbError;
            return std::nullopt;
        }
    }
    m_db.commit();

    TusVariableRow r;
    r.ownerUserId = vuser ? 0 : owner;
    r.slotId = slot;
    r.set = true;
    r.variable = finalVal;
    r.oldVariable = existed ? curVal : 0;
    r.lastChanged = conditionMet ? now : curDate;
    r.lastChangedAuthorId = conditionMet ? authorId : curAuthor;
    status = AddStatus::Ok;
    return r;
}

bool TusDb::SetData(const QString& comId, int64_t owner, int32_t slot, const QByteArray& data,
                    const QByteArray& info, int64_t authorId, uint64_t now) {
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tus_data(communication_id,owner_user_id,slot_id,data,info,data_size,"
              "last_changed,last_changed_author_id) VALUES(?,?,?,?,?,?,?,?) "
              "ON CONFLICT(communication_id,owner_user_id,slot_id) DO UPDATE SET "
              "data=excluded.data,info=excluded.info,data_size=excluded.data_size,"
              "last_changed=excluded.last_changed,last_changed_author_id=excluded.last_changed_"
              "author_id");
    q.addBindValue(comId);
    q.addBindValue(static_cast<qint64>(owner));
    q.addBindValue(slot);
    q.addBindValue(data);
    q.addBindValue(info);
    q.addBindValue(static_cast<qint64>(data.size()));
    q.addBindValue(static_cast<qint64>(now));
    q.addBindValue(static_cast<qint64>(authorId));
    return q.exec();
}

bool TusDb::SetVUserData(const QString& comId, const QString& virtualUser, int32_t slot,
                         const QByteArray& data, const QByteArray& info, int64_t authorId,
                         uint64_t now) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO tus_vuser_data(communication_id,virtual_user,slot_id,data,info,data_size,"
        "last_changed,last_changed_author_id) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(communication_id,virtual_user,slot_id) DO UPDATE SET "
        "data=excluded.data,info=excluded.info,data_size=excluded.data_size,"
        "last_changed=excluded.last_changed,last_changed_author_id=excluded.last_changed_author_"
        "id");
    q.addBindValue(comId);
    q.addBindValue(virtualUser);
    q.addBindValue(slot);
    q.addBindValue(data);
    q.addBindValue(info);
    q.addBindValue(static_cast<qint64>(data.size()));
    q.addBindValue(static_cast<qint64>(now));
    q.addBindValue(static_cast<qint64>(authorId));
    return q.exec();
}

std::optional<TusDataRow> TusDb::GetData(const QString& comId, int64_t owner, int32_t slot,
                                         bool withPayload) {
    QSqlQuery q(m_db);
    if (withPayload) {
        q.prepare("SELECT data,info,data_size,last_changed,last_changed_author_id FROM tus_data "
                  "WHERE communication_id=? AND owner_user_id=? AND slot_id=?");
    } else {
        q.prepare("SELECT info,data_size,last_changed,last_changed_author_id FROM tus_data "
                  "WHERE communication_id=? AND owner_user_id=? AND slot_id=?");
    }
    q.addBindValue(comId);
    q.addBindValue(static_cast<qint64>(owner));
    q.addBindValue(slot);
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    TusDataRow r;
    r.ownerUserId = owner;
    r.slotId = slot;
    r.set = true;
    int i = 0;
    if (withPayload) {
        r.data = q.value(i++).toByteArray();
    }
    r.info = q.value(i++).toByteArray();
    r.dataSize = q.value(i++).toULongLong();
    r.lastChanged = q.value(i++).toULongLong();
    r.lastChangedAuthorId = q.value(i++).toLongLong();
    return r;
}

std::optional<TusDataRow> TusDb::GetVUserData(const QString& comId, const QString& virtualUser,
                                              int32_t slot, bool withPayload) {
    QSqlQuery q(m_db);
    if (withPayload) {
        q.prepare("SELECT data,info,data_size,last_changed,last_changed_author_id FROM "
                  "tus_vuser_data WHERE communication_id=? AND virtual_user=? AND slot_id=?");
    } else {
        q.prepare("SELECT info,data_size,last_changed,last_changed_author_id FROM tus_vuser_data "
                  "WHERE communication_id=? AND virtual_user=? AND slot_id=?");
    }
    q.addBindValue(comId);
    q.addBindValue(virtualUser);
    q.addBindValue(slot);
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    TusDataRow r;
    r.slotId = slot;
    r.set = true;
    int i = 0;
    if (withPayload) {
        r.data = q.value(i++).toByteArray();
    }
    r.info = q.value(i++).toByteArray();
    r.dataSize = q.value(i++).toULongLong();
    r.lastChanged = q.value(i++).toULongLong();
    r.lastChangedAuthorId = q.value(i++).toLongLong();
    return r;
}

QVector<TusVariableRow> TusDb::GetVariables(const QString& comId, int64_t owner,
                                            const QVector<int32_t>& slotIds) {
    QVector<TusVariableRow> out;
    out.reserve(slotIds.size());
    for (int32_t slot : slotIds) {
        TusVariableRow r;
        r.ownerUserId = owner;
        r.slotId = slot;
        QSqlQuery q(m_db);
        q.prepare("SELECT variable,last_changed,last_changed_author_id FROM tus_variable "
                  "WHERE communication_id=? AND owner_user_id=? AND slot_id=?");
        q.addBindValue(comId);
        q.addBindValue(static_cast<qint64>(owner));
        q.addBindValue(slot);
        if (q.exec() && q.next()) {
            r.set = true;
            r.variable = q.value(0).toLongLong();
            r.lastChanged = q.value(1).toULongLong();
            r.lastChangedAuthorId = q.value(2).toLongLong();
        }
        out.append(r);
    }
    return out;
}

QVector<TusVariableRow> TusDb::GetVUserVariables(const QString& comId, const QString& virtualUser,
                                                 const QVector<int32_t>& slotIds) {
    QVector<TusVariableRow> out;
    out.reserve(slotIds.size());
    for (int32_t slot : slotIds) {
        TusVariableRow r;
        r.ownerUserId = 0; // virtual users have no account id
        r.slotId = slot;
        QSqlQuery q(m_db);
        q.prepare("SELECT variable,last_changed,last_changed_author_id FROM tus_vuser_variable "
                  "WHERE communication_id=? AND virtual_user=? AND slot_id=?");
        q.addBindValue(comId);
        q.addBindValue(virtualUser);
        q.addBindValue(slot);
        if (q.exec() && q.next()) {
            r.set = true;
            r.variable = q.value(0).toLongLong();
            r.lastChanged = q.value(1).toULongLong();
            r.lastChangedAuthorId = q.value(2).toLongLong();
        }
        out.append(r);
    }
    return out;
}
QVector<TusDataRow> TusDb::GetDataStatuses(const QString& comId,
                                           const QVector<QPair<int64_t, int32_t>>& ownerSlotPairs) {
    QVector<TusDataRow> out;
    out.reserve(ownerSlotPairs.size());
    for (const auto& p : ownerSlotPairs) {
        auto row = GetData(comId, p.first, p.second, /*withPayload=*/false);
        if (row) {
            out.append(*row);
        } else {
            TusDataRow r;
            r.ownerUserId = p.first;
            r.slotId = p.second;
            r.set = false;
            out.append(r);
        }
    }
    return out;
}

QVector<TusDataRow> TusDb::GetVUserDataStatuses(const QString& comId, const QString& virtualUser,
                                                const QVector<int32_t>& slotIds) {
    QVector<TusDataRow> out;
    out.reserve(slotIds.size());
    for (int32_t slot : slotIds) {
        auto row = GetVUserData(comId, virtualUser, slot, /*withPayload=*/false);
        if (row) {
            out.append(*row);
        } else {
            TusDataRow r;
            r.slotId = slot;
            r.set = false;
            out.append(r);
        }
    }
    return out;
}

bool TusDb::DeleteSlots(const QString& comId, int64_t owner, const QVector<int32_t>& slotIds) {
    // DeleteMultiSlotData deletes TUS *data* only. the variable in the same slot is
    // untouched
    WriteTxn txn(m_db);
    if (!txn.Ok()) {
        return false;
    }
    for (int32_t slot : slotIds) {
        QSqlQuery qd(m_db);
        qd.prepare("DELETE FROM tus_data WHERE communication_id=? AND owner_user_id=? AND "
                   "slot_id=?");
        qd.addBindValue(comId);
        qd.addBindValue(static_cast<qint64>(owner));
        qd.addBindValue(slot);
        if (!qd.exec()) {
            return false;
        }
    }
    return txn.Commit();
}

bool TusDb::DeleteVUserSlots(const QString& comId, const QString& virtualUser,
                             const QVector<int32_t>& slotIds) {
    // Data-only (vuser). the vuser variable in the same slot is left intact.
    WriteTxn txn(m_db);
    if (!txn.Ok()) {
        return false;
    }
    for (int32_t slot : slotIds) {
        QSqlQuery qd(m_db);
        qd.prepare("DELETE FROM tus_vuser_data WHERE communication_id=? AND virtual_user=? AND "
                   "slot_id=?");
        qd.addBindValue(comId);
        qd.addBindValue(virtualUser);
        qd.addBindValue(slot);
        if (!qd.exec()) {
            return false;
        }
    }
    return txn.Commit();
}

bool TusDb::DeleteVariableSlots(const QString& comId, int64_t owner,
                                const QVector<int32_t>& slotIds) {
    WriteTxn txn(m_db);
    if (!txn.Ok()) {
        return false;
    }
    for (int32_t slot : slotIds) {
        QSqlQuery q(m_db);
        q.prepare("DELETE FROM tus_variable WHERE communication_id=? AND owner_user_id=? AND "
                  "slot_id=?");
        q.addBindValue(comId);
        q.addBindValue(static_cast<qint64>(owner));
        q.addBindValue(slot);
        if (!q.exec()) {
            return false;
        }
    }
    return txn.Commit();
}

bool TusDb::DeleteVUserVariableSlots(const QString& comId, const QString& virtualUser,
                                     const QVector<int32_t>& slotIds) {
    WriteTxn txn(m_db);
    if (!txn.Ok()) {
        return false;
    }
    for (int32_t slot : slotIds) {
        QSqlQuery q(m_db);
        q.prepare("DELETE FROM tus_vuser_variable WHERE communication_id=? AND virtual_user=? AND "
                  "slot_id=?");
        q.addBindValue(comId);
        q.addBindValue(virtualUser);
        q.addBindValue(slot);
        if (!q.exec()) {
            return false;
        }
    }
    return txn.Commit();
}

std::optional<int64_t> TusDb::UserIdForNpid(const QString& npid) {
    QSqlQuery q(m_db);
    q.prepare("SELECT user_id FROM account WHERE username=? COLLATE NOCASE");
    q.addBindValue(npid);
    if (q.exec() && q.next()) {
        return q.value(0).toLongLong();
    }
    return std::nullopt;
}

std::optional<QString> TusDb::NpidForUserId(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT username FROM account WHERE user_id=?");
    q.addBindValue(static_cast<qint64>(userId));
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return std::nullopt;
}
