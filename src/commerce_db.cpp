// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "commerce_db.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

QString TitleKey(const QString& titleId) {
    return titleId.isNull() ? QStringLiteral("") : titleId;
}

} // namespace

bool CommerceDb::RecordRequested(const QString& titleId, int serviceLabel,
                                 const QStringList& labels, int64_t now) {
    if (labels.isEmpty())
        return true;
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qWarning() << "CommerceDb: cannot start transaction:" << m_lastError;
        return false;
    }
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO commerce_label(title_id,service_label,label,first_seen,last_seen,"
              "requests) VALUES(?,?,?,?,?,1)"
              " ON CONFLICT(title_id,service_label,label) DO UPDATE SET"
              " last_seen=excluded.last_seen, requests=requests+1");
    for (const QString& label : labels) {
        q.addBindValue(TitleKey(titleId));
        q.addBindValue(serviceLabel);
        q.addBindValue(label);
        q.addBindValue(static_cast<qlonglong>(now));
        q.addBindValue(static_cast<qlonglong>(now));
        if (!q.exec()) {
            m_lastError = q.lastError().text();
            qWarning() << "CommerceDb: record failed:" << m_lastError;
            m_db.rollback();
            return false;
        }
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        qWarning() << "CommerceDb: commit failed:" << m_lastError;
        m_db.rollback();
        return false;
    }
    return true;
}

QVector<CommerceLabel> CommerceDb::Query(const QString& where, const QVariantList& binds) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT title_id,service_label,label,first_seen,last_seen,requests "
                             "FROM commerce_label %1")
                  .arg(where));
    for (const QVariant& b : binds)
        q.addBindValue(b);
    QVector<CommerceLabel> out;
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qWarning() << "CommerceDb: query failed:" << m_lastError;
        return out;
    }
    while (q.next()) {
        CommerceLabel l;
        l.titleId = q.value(0).toString();
        l.serviceLabel = q.value(1).toInt();
        l.label = q.value(2).toString();
        l.firstSeen = q.value(3).toLongLong();
        l.lastSeen = q.value(4).toLongLong();
        l.requests = q.value(5).toLongLong();
        out.append(l);
    }
    return out;
}

QVector<CommerceLabel> CommerceDb::ForTitle(const QString& titleId, int serviceLabel) {
    return Query(QStringLiteral("WHERE title_id=? AND service_label=? ORDER BY label"),
                 {TitleKey(titleId), serviceLabel});
}

QVector<CommerceLabel> CommerceDb::All(const QString& titleId) {
    if (titleId.isEmpty())
        return Query(QStringLiteral("ORDER BY title_id, service_label, label"), {});
    return Query(QStringLiteral("WHERE title_id=? ORDER BY service_label, label"), {titleId});
}
