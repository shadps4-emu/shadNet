// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "auth_code_store.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QRandomGenerator>

namespace {

constexpr int CodeLength = 6;

QString GenerateCode() {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    QString code;
    code.reserve(CodeLength);
    for (int i = 0; i < CodeLength; ++i) {
        code +=
            QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]);
    }
    return code;
}

} // namespace

std::optional<QString> AuthCodeStore::Issue(Grant grant) {
    QMutexLocker lk(&m_mutex);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    PurgeExpired(now);
    int outstanding = 0;
    for (const Grant& g : std::as_const(m_grants)) {
        if (g.userId == grant.userId) {
            ++outstanding;
        }
    }
    if (outstanding >= MaxOutstandingPerUser) {
        return std::nullopt;
    }
    grant.issuedAt = now;
    grant.expiresAt = now + LifetimeSecs;
    QString code;
    do {
        code = GenerateCode();
    } while (m_grants.contains(code));
    m_grants.insert(code, std::move(grant));
    return code;
}

std::optional<AuthCodeStore::Grant> AuthCodeStore::Redeem(const QString& code) {
    QMutexLocker lk(&m_mutex);
    PurgeExpired(QDateTime::currentSecsSinceEpoch());
    const auto it = m_grants.find(code);
    if (it == m_grants.end()) {
        return std::nullopt;
    }
    Grant grant = it.value();
    m_grants.erase(it);
    return grant;
}

void AuthCodeStore::PurgeExpired(qint64 now) {
    for (auto it = m_grants.begin(); it != m_grants.end();) {
        it = it->expiresAt <= now ? m_grants.erase(it) : std::next(it);
    }
}
