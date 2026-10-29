// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

struct Entitlement {
    int64_t userId = 0;
    QString titleId; // "" = every title
    int serviceLabel = 0;
    QString label;                            // entitlement label, e.g. CALLINGALLCARS01
    QString type = QStringLiteral("service"); // "service" or "unified"
    int64_t activeDate = 0;                   // unix seconds
    std::optional<int64_t> inactiveDate;      // unix seconds, nullopt = no end date
    bool consumable = false;
    int64_t useCount = 0;
    int64_t useLimit = 0;
    QString grantedBy;
    int64_t grantedAt = 0;

    bool IsActive(int64_t now) const {
        return activeDate <= now && (!inactiveDate || now < *inactiveDate);
    }
};

enum class ConsumeError { NotFound, NotConsumable, Inactive, InsufficientUses, Internal };

class EntitlementDb {
public:
    explicit EntitlementDb(const QSqlDatabase& db) : m_db(db) {}
    QVector<Entitlement> ListVisible(int64_t userId, const QString& titleId, int serviceLabel);
    std::optional<Entitlement> GetVisible(int64_t userId, const QString& titleId, int serviceLabel,
                                          const QString& label);
    std::optional<int64_t> Consume(int64_t userId, const QString& titleId, int serviceLabel,
                                   const QString& label, int64_t count, int64_t now,
                                   ConsumeError& error);
    QVector<Entitlement> ListAll(int64_t userId);
    bool Upsert(const Entitlement& e);
    bool Remove(int64_t userId, const QString& titleId, int serviceLabel, const QString& label);

    QString LastError() const {
        return m_lastError;
    }

private:
    QVector<Entitlement> Query(const QString& where, const QVariantList& binds);
    QSqlDatabase m_db;
    QString m_lastError;
};
