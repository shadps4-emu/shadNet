// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QReadWriteLock>
#include <QString>
#include <QStringList>
#include <QVector>
#include "clientversionsconfig.h"

class ClientVersionRules {
public:
    struct Decision {
        bool allowed = true;
        QString message; // for a denial
        int line = 0;    // the rule that decided, 0 = no rule matched
    };
    bool Load(const QString& path, QStringList& errors);
    void Apply(const QString& content, QStringList& errors);
    Decision Evaluate(const QString& version) const;
    int RuleCount() const;

    static QString DefaultFileContent();

private:
    mutable QReadWriteLock m_lock;
    QVector<ClientVersionRule> m_rules;
};
