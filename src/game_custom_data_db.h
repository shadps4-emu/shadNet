// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <QByteArray>
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

struct GameCustomDataScope {
    QString communicationId;
    QString titleId;
};

struct GameCustomDataItem {
    int64_t itemId = 0;
    int64_t fromUserId = 0;
    QString fromNpid;
    QString communicationId;
    QString titleId;
    QString message;
    QString dataName;
    QString dataDescription;
    QHash<QString, QString> localizedNames;              // npLanguage -> name
    QHash<QString, QString> localizedDescriptions;       // npLanguage -> description
    QString attachmentType = QStringLiteral("gameData"); // "gameData" or "url"
    QString url;                                         // final URL (itemId already appended)
    QStringList platforms;                               // "PS4", "PSVITA"
    int64_t createdAt = 0;                               // unix seconds
    std::optional<int64_t> expiresAt;                    // unix seconds, nullopt = never
    bool used = false;                                   // this recipient's flag

    bool IsUrl() const {
        return attachmentType == QStringLiteral("url");
    }
    bool IsExpired(int64_t now) const {
        return expiresAt && now >= *expiresAt;
    }
};

class GameCustomDataDb {
public:
    // The server keeps at most this many received items per user, older ones are dropped.
    static constexpr int kMaxReceivedPerUser = 100;

    explicit GameCustomDataDb(const QSqlDatabase& db) : m_db(db) {}
    std::optional<int64_t> Create(GameCustomDataItem item, bool withItemId,
                                  const QByteArray& gameData, const QByteArray& thumbnail,
                                  const QVector<int64_t>& recipients);

    // Items received by `userId` in the caller's game, newest first.
    QVector<GameCustomDataItem> ListReceived(int64_t userId, const GameCustomDataScope& scope);
    std::optional<GameCustomDataItem> GetReceived(int64_t userId, int64_t itemId,
                                                  const GameCustomDataScope& scope);
    std::optional<QByteArray> GameData(int64_t itemId);
    std::optional<QByteArray> Thumbnail(int64_t itemId);
    bool MarkUsed(int64_t userId, int64_t itemId);

    QString LastError() const {
        return m_lastError;
    }

private:
    void PruneReceived(int64_t userId);
    QSqlDatabase m_db;
    QString m_lastError;
};
