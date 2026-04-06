// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include "protocol.h"

// ── Friendship ────────────────────────────────────────────────────────────────
// Status flags stored per-user per-row in the friendship table.
// The Friend bit represents "this user has either sent or confirmed a request".
// When both sides have Friend=1 the relationship is a confirmed mutual friendship.
// When only one side has Friend=1 there is an open outgoing request from that side.
enum class FriendStatus : uint8_t {
    Friend = (1 << 0),
    Blocked = (1 << 1),
};

struct UserRelationships {
    QList<QPair<int64_t, QString>> friends;                // mutual (both Friend bits set)
    QList<QPair<int64_t, QString>> friendRequestsSent;     // we sent, they haven't replied
    QList<QPair<int64_t, QString>> friendRequestsReceived; // they sent, we haven't replied
    QList<QPair<int64_t, QString>> blocked;                // we blocked them
};

struct UserRecord {
    int64_t userId = 0;
    QString username;
    QByteArray hash;
    QByteArray salt;
    QString onlineName;
    QString avatarUrl;
    QString email;
    QString emailCheck;
    QString token;
    bool admin = false;
    bool statAgent = false;
    bool banned = false;
};

enum class DbError {
    None = 0,         // No error (success)
    ExistingUsername, // Username already exists
    ExistingEmail,    // Email already exists
    InvalidInput,     // Invalid input parameters
    InvalidEmail,     // Invalid email format
    Internal,         // Internal database error
    ConnectionError,  // Database connection error
    TransactionError  // Transaction failed
};

class Database {
public:
    explicit Database(const QString& connectionName = "default");
    ~Database();

    bool Open(const QString& path = "db/rpcn.db");
    bool Migrate();
    bool IsOpen() const;
    QSqlDatabase Conn() const {
        return m_db;
    }

    // Account
    std::optional<DbError> CreateAccount(const QString& npid, const QString& password,
                                         const QString& onlineName, const QString& avatarUrl,
                                         const QString& email);
    std::optional<UserRecord> CheckUser(const QString& npid, const QString& password,
                                        const QString& token, bool checkToken);
    std::optional<int64_t> GetUserId(const QString& npid);
    std::optional<QString> GetUsername(int64_t userId);
    QList<QPair<int64_t, QString>> GetUsernamesFromIds(const QSet<int64_t>& ids);
    bool UpdateLoginTime(int64_t userId);
    bool BanUser(int64_t userId, bool ban);
    bool DeleteUser(int64_t userId);
    bool SetAdmin(int64_t userId, bool admin);
    int TotalUsers();
    void CleanNeverUsedAccounts();

    // Friendship
    // Returns (status_caller, status_other). Empty = no row exists yet.
    enum class RelResult { Ok, Empty, Error };
    struct RelStatus {
        uint8_t caller = 0;
        uint8_t other = 0;
    };
    std::pair<RelResult, RelStatus> GetRelStatus(int64_t callerId, int64_t otherId);
    bool SetRelStatus(int64_t callerId, int64_t otherId, uint8_t statusCaller, uint8_t statusOther);
    bool DeleteRel(int64_t callerId, int64_t otherId);
    UserRelationships GetRelationships(int64_t userId);

    QString lastError() const {
        return m_lastError;
    }

private:
    bool Exec(const QString& sql);
    bool Exec(QSqlQuery& q);
    QByteArray HashPassword(const QString& password, const QByteArray& salt);
    QByteArray GenerateSalt(int bytes = 64);
    QString GenerateToken(int len = 16);

    QSqlDatabase m_db;
    QString m_connName;
    mutable QString m_lastError;
};