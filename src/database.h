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
    QString avatarUrl;
    QString email;
    QString emailCheck;
    QString token;
    bool admin = false;
    bool statAgent = false;
    bool banned = false;
};

// ── Admin tooling
struct AdminUserRow {
    int64_t userId = 0;
    QString username;
    QString email;
    bool admin = false;
    bool statAgent = false;
    bool banned = false;
    QString banReason;
    int64_t banTimestamp = 0; // seconds since epoch, 0 when never banned
    int64_t creation = 0;     // seconds since epoch, 0 when unknown
    int64_t lastLogin = 0;    // seconds since epoch, 0 when never logged in
    QString clientVersion;
    int64_t clientVersionAt = 0;
};

// Which accounts ListUsers/CountUsers should return.
enum class UserFilter {
    All,
    BannedOnly,
    AdminsOnly,
    ActiveOnly,   // not banned
    WithScores,   // has posted at least one leaderboard score
    WithTrophies, // has unlocked at least one trophy
    OnlineOnly,   // currently connected
};

// One entry of the admin action log.
struct AuditRow {
    int64_t id = 0;
    int64_t timestamp = 0;
    int64_t actorUserId = 0;
    QString actorNpid;
    QString action;
    int64_t targetUserId = 0;
    QString targetNpid;
    QString reason;
};

// What a PurgeUserData call removed.
struct PurgeSummary {
    int scores = 0;
    int trophies = 0;
    int tusVariables = 0;
    int tusData = 0;
    int friendships = 0;
    // data_id of every score blob that belonged to the purged rows.
    QList<uint64_t> scoreDataIds;

    int total() const {
        return scores + trophies + tusVariables + tusData + friendships;
    }
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
                                         const QString& avatarUrl, const QString& email);
    std::optional<UserRecord> CheckUser(const QString& npid, const QString& password,
                                        const QString& token, bool checkToken);
    std::optional<int64_t> GetUserId(const QString& npid);
    std::optional<QString> GetUsername(int64_t userId);
    std::optional<QString> GetAvatarUrl(int64_t userId);
    // Account creation instant, in seconds since the epoch (account_timestamp.creation).
    std::optional<int64_t> GetAccountCreationTime(int64_t userId);
    QList<QPair<int64_t, QString>> GetUsernamesFromIds(const QSet<int64_t>& ids);
    bool UpdateLoginTime(int64_t userId);
    // Sets/clears the ban flag.
    bool BanUser(int64_t userId, bool ban, const QString& reason = QString());

    // Removes the account row itself along with everything PurgeUserData covers
    bool DeleteAccount(int64_t userId, PurgeSummary& summary);
    bool SetAdmin(int64_t userId, bool admin);
    bool SetClientVersion(int64_t userId, const QString& version);

    // Replaces an account's password. Generates a fresh salt, and rotates the
    // account token so any client still holding the old one has to sign in again —
    // a password reset that left existing sessions working would not be much of a
    // reset. Also clears any pending reset_token.
    bool SetPassword(int64_t userId, const QString& newPassword);
    // Points an account at a different picture.
    bool SetAvatarUrl(int64_t userId, const QString& avatarUrl);
    int TotalUsers();

    // Deletes everything this account produced: leaderboard scores, TUS variable
    // and data slots, and friend/block relationships.
    bool PurgeUserData(int64_t userId, PurgeSummary& summary);

    QList<AdminUserRow> ListUsers(
        const QString& search, UserFilter filter, int limit, int offset,
        const std::optional<QList<int64_t>>& restrictToIds = std::nullopt);
    int CountUsers(const QString& search, UserFilter filter,
                   const std::optional<QList<int64_t>>& restrictToIds = std::nullopt);
    std::optional<AdminUserRow> GetUserRow(int64_t userId);
    int CountUsersWhere(UserFilter filter);

    // Append an entry to the admin action log. Returns false only on a DB error.
    bool AddAuditEntry(int64_t actorUserId, const QString& actorNpid, const QString& action,
                       int64_t targetUserId, const QString& targetNpid, const QString& reason);
    QList<AuditRow> ListAudit(int limit, int offset);

    void CleanNeverUsedAccounts();
    void RunMaintenance();

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

    bool SetTitleName(const QString& comId, const QString& titleName);
    std::optional<QString> GetTitleName(const QString& comId);

    // Row returned by ListScoredGameTitles.
    struct GameTitleRow {
        QString comId;
        QString titleName;
    };
    QList<GameTitleRow> ListScoredGameTitles();

    // Game titles
    struct KnownTitleRow {
        QString comId;
        QString titleName;
        bool hasScores = false;
        bool hasTrophies = false;
        bool hasTrophyNames = false;
    };
    QList<KnownTitleRow> ListKnownTitles();
    bool RenameTitle(const QString& comId, const QString& titleName);
    bool ClearTitleName(const QString& comId);

    // Leaderboard moderation
    // One board that currently holds scores.
    struct BoardRow {
        QString comId;
        QString titleName;
        uint32_t boardId = 0;
        int scoreCount = 0;
    };
    // Every (com id, board) pair with at least one score, most populated first.
    QList<BoardRow> ListScoreBoards();

    // One posted score, joined to the account that posted it.
    struct BoardScoreRow {
        QString comId;
        uint32_t boardId = 0;
        int64_t userId = 0;
        QString npid; // empty when the account no longer exists
        int32_t characterId = 0;
        int64_t score = 0;
        QString comment;
        uint64_t dataId = 0; // 0 when the entry has no saved game data
        int64_t timestamp = 0;
    };
    // Scores on one board, highest first.
    QList<BoardScoreRow> ListBoardScores(const QString& comId, uint32_t boardId, int limit,
                                         int offset);
    int CountBoardScores(const QString& comId, uint32_t boardId);

    // Removes exactly one posted score.
    bool DeleteScore(const QString& comId, uint32_t boardId, int64_t userId, int32_t characterId,
                     uint64_t& dataId);

    // Trophies
    // One trophy a player has unlocked.
    struct TrophyRow {
        QString comId;
        QString titleName;
        int32_t trophyId = 0;
        int64_t earnedAt = 0;
    };
    // A game somebody has earned trophies in, with how widely.
    struct TrophyGameRow {
        QString comId;
        QString titleName;
        int players = 0;  // distinct accounts holding at least one trophy
        int trophies = 0; // distinct trophy ids earned at least once
        int unlocks = 0;  // total unlock records
    };
    // How many accounts hold a given trophy in a game.
    struct TrophyEarnerRow {
        int32_t trophyId = 0;
        int earners = 0;
    };

    // Records an unlock.
    bool RecordUserTrophy(int64_t userId, const QString& comId, int32_t trophyId, int64_t earnedAt);
    bool RecordUserTrophies(int64_t userId, const QString& comId,
                            const QList<QPair<int32_t, int64_t>>& trophies);

    // Every trophy one account holds, ordered by game then trophy id.
    QList<TrophyRow> ListUserTrophies(int64_t userId);
    // Trophies one account holds in one game.
    QList<TrophyRow> ListUserTrophiesForGame(int64_t userId, const QString& comId);
    // Games with any trophy activity, most played first.
    QList<TrophyGameRow> ListTrophyGames();
    // Earner counts per trophy for one game — the input to "earned by X%".
    QList<TrophyEarnerRow> ListTrophyEarners(const QString& comId);

    struct TrophySetShape {
        int total = 0;
        int bronze = 0;
        int silver = 0;
        int gold = 0;
        int platinum = 0;
    };
    TrophySetShape GetTrophySetShape(const QString& comId);
    int CountTrophyPlayers(const QString& comId);

    // A player ranked by how many trophies they hold in one game.
    struct TrophyPlayerRow {
        int64_t userId = 0;
        QString npid;
        int trophies = 0;
        int64_t lastEarnedAt = 0;
    };
    // Top trophy holders for a game, most trophies first.
    QList<TrophyPlayerRow> ListTopTrophyPlayers(const QString& comId, int limit);

    // One game in a player's trophy profile.
    struct TrophyProfileRow {
        QString comId;
        QString titleName;
        int trophies = 0;
        int64_t firstEarnedAt = 0;
        int64_t lastEarnedAt = 0;
        int bronze = 0;
        int silver = 0;
        int gold = 0;
        int platinum = 0;
        int unknownGrade = 0;
        int totalInGame = 0;
    };
    // Per-game breakdown for one account, most trophies first.
    QList<TrophyProfileRow> ListPlayerTrophySummary(int64_t userId);

    // Server-wide trophy totals.
    struct TrophyTotals {
        int players = 0; // accounts holding at least one trophy
        int games = 0;   // games with any trophy activity
        int unlocks = 0; // total unlock records
    };
    TrophyTotals GetTrophyTotals();

    // Trophy metadata
    struct TrophyMetaRow {
        QString comId;
        int32_t trophyId = 0;
        QString name;
        QString detail;
        QString grade; // "B", "S", "G" or "P"
        bool hidden = false;
        int32_t groupId = 0;
        QString language; // which TROP_xx.XML it came from; empty for the master
    };

    // A trophy group: the base game, or a DLC pack. Trophies carry a groupId
    // that points here, so DLC can be shown as its own section.
    struct TrophyGroupRow {
        int32_t groupId = 0;
        QString name;
        QString detail;
    };
    QList<TrophyGroupRow> ListTrophyGroups(const QString& comId);

    bool ImportTrophyMeta(const QString& comId, const QList<TrophyMetaRow>& rows,
                          const QList<TrophyGroupRow>& groups = {});
    QList<TrophyMetaRow> ListTrophyMeta(const QString& comId);
    int CountTrophyMeta(const QString& comId);
    bool DeleteTrophyMeta(const QString& comId);
    bool DeleteUserTrophy(int64_t userId, const QString& comId, int32_t trophyId);

    QString lastError() const {
        return m_lastError;
    }

private:
    bool Exec(const QString& sql);
    bool Exec(QSqlQuery& q);
    bool HasMigration(int id);
    bool PurgeUserDataStatements(int64_t userId, PurgeSummary& summary);
    void CollectScoreDataIds(int64_t userId, PurgeSummary& summary);
    static QString BuildUserFilterClause(const QString& search, UserFilter filter,
                                         const std::optional<QList<int64_t>>& restrictToIds);
    static void BindUserFilter(QSqlQuery& q, const QString& search,
                               const std::optional<QList<int64_t>>& restrictToIds);
    QByteArray HashPassword(const QString& password, const QByteArray& salt);
    QByteArray GenerateSalt(int bytes = 64);
    QString GenerateToken(int len = 16);

    QSqlDatabase m_db;
    QString m_connName;
    mutable QString m_lastError;
};