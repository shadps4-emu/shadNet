// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "entitlement_db.h"

#include <algorithm>
#include <QDebug>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr const char* kColumns =
    "user_id,title_id,service_label,label,entitlement_type,active_date,inactive_date,"
    "is_consumable,use_count,use_limit,granted_by,granted_at";

QString TitleKey(const QString& titleId) {
    return titleId.isNull() ? QStringLiteral("") : titleId;
}

Entitlement FromRow(const QSqlQuery& q) {
    Entitlement e;
    e.userId = q.value(0).toLongLong();
    e.titleId = q.value(1).toString();
    e.serviceLabel = q.value(2).toInt();
    e.label = q.value(3).toString();
    e.type = q.value(4).toString();
    e.activeDate = q.value(5).toLongLong();
    if (!q.value(6).isNull())
        e.inactiveDate = q.value(6).toLongLong();
    e.consumable = q.value(7).toInt() != 0;
    e.useCount = q.value(8).toLongLong();
    e.useLimit = q.value(9).toLongLong();
    e.grantedBy = q.value(10).toString();
    e.grantedAt = q.value(11).toLongLong();
    return e;
}

bool SortByActiveDate(const Entitlement& a, const Entitlement& b) {
    if (a.activeDate != b.activeDate)
        return a.activeDate < b.activeDate;
    return a.label < b.label;
}

} // namespace

QVector<Entitlement> EntitlementDb::Query(const QString& where, const QVariantList& binds) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM entitlement WHERE %2")
                  .arg(QString::fromLatin1(kColumns), where));
    for (const QVariant& b : binds)
        q.addBindValue(b);
    QVector<Entitlement> out;
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "EntitlementDb: query failed:" << m_lastError;
        return out;
    }
    while (q.next())
        out.append(FromRow(q));
    return out;
}

QVector<Entitlement> EntitlementDb::ListVisible(int64_t userId, const QString& titleId,
                                                int serviceLabel) {
    const auto rows = Query(QStringLiteral("user_id=? AND service_label=? AND title_id IN (?, '')"),
                            {static_cast<qlonglong>(userId), serviceLabel, TitleKey(titleId)});
    // One row per label; a row for this title shadows an all-titles row.
    QHash<QString, Entitlement> byLabel;
    for (const Entitlement& e : rows) {
        auto it = byLabel.find(e.label);
        if (it == byLabel.end() || (it->titleId.isEmpty() && !e.titleId.isEmpty()))
            byLabel.insert(e.label, e);
    }
    QVector<Entitlement> out = byLabel.values().toVector();
    std::sort(out.begin(), out.end(), SortByActiveDate);
    return out;
}

std::optional<Entitlement> EntitlementDb::GetVisible(int64_t userId, const QString& titleId,
                                                     int serviceLabel, const QString& label) {
    // Title-specific first (non-empty title_id sorts after ''), so take the last row.
    const auto rows =
        Query(QStringLiteral("user_id=? AND service_label=? AND label=? AND "
                             "title_id IN (?, '') ORDER BY title_id"),
              {static_cast<qlonglong>(userId), serviceLabel, label, TitleKey(titleId)});
    if (rows.isEmpty())
        return std::nullopt;
    return rows.last();
}

std::optional<int64_t> EntitlementDb::Consume(int64_t userId, const QString& titleId,
                                              int serviceLabel, const QString& label, int64_t count,
                                              int64_t now, ConsumeError& error) {
    const auto row = GetVisible(userId, titleId, serviceLabel, label);
    if (!row) {
        error = ConsumeError::NotFound;
        return std::nullopt;
    }
    if (!row->consumable) {
        error = ConsumeError::NotConsumable;
        return std::nullopt;
    }
    if (!row->IsActive(now)) {
        error = ConsumeError::Inactive;
        return std::nullopt;
    }

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "EntitlementDb: consume cannot start transaction:" << m_lastError;
        error = ConsumeError::Internal;
        return std::nullopt;
    }
    QSqlQuery q(m_db);
    q.prepare("UPDATE entitlement SET use_count=use_count+?, use_limit=use_limit-?"
              " WHERE user_id=? AND title_id=? AND service_label=? AND label=?"
              " AND is_consumable=1 AND use_limit>=? AND active_date<=?"
              " AND (inactive_date IS NULL OR inactive_date>?)");
    q.addBindValue(static_cast<qlonglong>(count));
    q.addBindValue(static_cast<qlonglong>(count));
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(TitleKey(row->titleId));
    q.addBindValue(serviceLabel);
    q.addBindValue(label);
    q.addBindValue(static_cast<qlonglong>(count));
    q.addBindValue(static_cast<qlonglong>(now));
    q.addBindValue(static_cast<qlonglong>(now));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "EntitlementDb: consume failed:" << m_lastError;
        m_db.rollback();
        error = ConsumeError::Internal;
        return std::nullopt;
    }
    if (q.numRowsAffected() == 0) {
        m_db.rollback();
        error = ConsumeError::InsufficientUses;
        return std::nullopt;
    }

    QSqlQuery r(m_db);
    r.prepare("SELECT use_limit FROM entitlement WHERE user_id=? AND title_id=? AND "
              "service_label=? AND label=?");
    r.addBindValue(static_cast<qlonglong>(userId));
    r.addBindValue(TitleKey(row->titleId));
    r.addBindValue(serviceLabel);
    r.addBindValue(label);
    if (!r.exec() || !r.next()) {
        m_lastError = r.lastError().text();
        qCritical() << "EntitlementDb: consume readback failed:" << m_lastError;
        m_db.rollback();
        error = ConsumeError::Internal;
        return std::nullopt;
    }
    const int64_t remaining = r.value(0).toLongLong();
    r.finish();
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "EntitlementDb: consume commit failed:" << m_lastError;
        m_db.rollback();
        error = ConsumeError::Internal;
        return std::nullopt;
    }
    return remaining;
}

QVector<Entitlement> EntitlementDb::ListAll(int64_t userId) {
    auto rows = Query(QStringLiteral("user_id=? ORDER BY title_id, service_label, label"),
                      {static_cast<qlonglong>(userId)});
    return rows;
}

bool EntitlementDb::Upsert(const Entitlement& e) {
    QSqlQuery q(m_db);
    q.prepare(
        QStringLiteral("INSERT OR REPLACE INTO entitlement(%1) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)")
            .arg(QString::fromLatin1(kColumns)));
    q.addBindValue(static_cast<qlonglong>(e.userId));
    q.addBindValue(TitleKey(e.titleId));
    q.addBindValue(e.serviceLabel);
    q.addBindValue(e.label);
    q.addBindValue(e.type);
    q.addBindValue(static_cast<qlonglong>(e.activeDate));
    q.addBindValue(e.inactiveDate ? QVariant(static_cast<qlonglong>(*e.inactiveDate))
                                  : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(e.consumable ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(e.useCount));
    q.addBindValue(static_cast<qlonglong>(e.useLimit));
    q.addBindValue(e.grantedBy);
    q.addBindValue(static_cast<qlonglong>(e.grantedAt));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "EntitlementDb: upsert failed:" << m_lastError;
        return false;
    }
    return true;
}

bool EntitlementDb::Remove(int64_t userId, const QString& titleId, int serviceLabel,
                           const QString& label) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM entitlement WHERE user_id=? AND title_id=? AND service_label=? AND "
              "label=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(TitleKey(titleId));
    q.addBindValue(serviceLabel);
    q.addBindValue(label);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "EntitlementDb: remove failed:" << m_lastError;
        return false;
    }
    return q.numRowsAffected() > 0;
}
