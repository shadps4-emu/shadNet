// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <QHash>
#include <QMutex>
#include <QString>

class AuthCodeStore {
public:
    struct Grant {
        int64_t userId = 0;
        QString clientId;
        QString scope;
        QString npTitleId;
        qint64 issuedAt = 0;
        qint64 expiresAt = 0;
    };
    static constexpr int LifetimeSecs = 300;
    static constexpr int MaxOutstandingPerUser = 16;
    // Returns the new code, or nullopt when the account already has too many unused codes.
    std::optional<QString> Issue(Grant grant);
    // Removes and returns the grant for code. Expired codes are treated as absent.
    std::optional<Grant> Redeem(const QString& code);

    // Server-to-server access token a game backend gets for a redeemed code.
    static constexpr int AccessTokenLifetimeSecs = 3599;
    QString IssueAccessToken(Grant grant);
    // The grant behind an unexpired access token.
    std::optional<Grant> FindAccessToken(const QString& token);

private:
    void PurgeExpired(qint64 now);

    QMutex m_mutex;
    QHash<QString, Grant> m_grants;
    QHash<QString, Grant> m_accessTokens;
};
