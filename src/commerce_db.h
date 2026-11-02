// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

// Store labels games have asked about through the Commerce Web API (GET Container).
//
// shadNet has no store catalog, so this is how an operator learns which labels a title uses:
// every product label a game requests is recorded per title serial and service label. The same
// labels are usually the entitlement labels that unlock the content, so this list is what to
// grant from. It also gives the title's top category its product links.
struct CommerceLabel {
    QString titleId; // title serial the request came from, "" when unknown
    int serviceLabel = 0;
    QString label;
    int64_t firstSeen = 0; // unix seconds
    int64_t lastSeen = 0;
    int64_t requests = 0;
};

class CommerceDb {
public:
    explicit CommerceDb(const QSqlDatabase& db) : m_db(db) {}
    bool RecordRequested(const QString& titleId, int serviceLabel, const QStringList& labels,
                         int64_t now);
    QVector<CommerceLabel> ForTitle(const QString& titleId, int serviceLabel);
    QVector<CommerceLabel> All(const QString& titleId = {});

    QString LastError() const {
        return m_lastError;
    }

private:
    QVector<CommerceLabel> Query(const QString& where, const QVariantList& binds);
    QSqlDatabase m_db;
    QString m_lastError;
};
