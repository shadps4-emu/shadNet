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

// ── Entitlement ───────────────────────────────────────────────────────────────
// One owned NpToolkit/np_commerce2 entitlement (consumable or unlimited DLC) for a
// single account. Multi-title: communicationId groups rows by owning title, but
// entitlementId is globally unique (reverse-DNS namespaced, e.g.
// "com.halfbrick.jetpack.CoinPack1"), so one account query returns every title's
// entitlements and each game filters by recognizing its own ids. Field roles map to
// the confirmed entitlement-webapi JSON: useLimit->remainingCount, useCount->consumedCount.
struct EntitlementRecord {
    QString communicationId;
    QString entitlementId;
    QString entitlementType = QStringLiteral("service");
    bool isConsumable = false;
    int serviceLabel = 0;  // NP Service Label (scoping; not part of the JSON object)
    int64_t useLimit = 0;
    int64_t useCount = 0;
    QString activeDate = QStringLiteral("2024-01-01T00:00:00Z");
    QString inactiveDate;  // empty = no expiry; omitted from the response when empty
};

// ── Commerce container product ────────────────────────────────────────────────
// One purchasable product within a commerce container (category). Reverse-engineered
// from title decompiles (no published schema): confirmed keys container_type/label/
// name/long_desc/provider_name/display_price/price/annotation/use_count and a default
// sku of type "standard". Children are nested under the container's "links" array.
struct ProductRecord {
    QString communicationId;
    int serviceLabel = 0;
    QString containerLabel;       // category this product belongs to
    QString label;                // product label (id)
    QString name;
    QString longDesc;
    QString providerName;
    int64_t price = 0;            // SKU price, minor units (e.g. 3999 = 39.99)
    int64_t originalPrice = 0;    // pre-discount price; 0 -> mirror price
    QString displayPrice;         // formatted, e.g. "39.99USD"
    QString displayOriginalPrice; // formatted original; empty -> mirror displayPrice
    int64_t useCount = 0;         // consumable count this product grants (SKU use_count)
    QString entitlementId;        // entitlement a purchase grants; empty -> product label
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

    // Entitlements (np_commerce2 / NpToolkit entitlement webapi). Rows are grouped by
    // communication_id for multi-title use; ids are globally unique so a per-account
    // query returns every title's entitlements (each game filters its own ids).
    enum class ConsumeResult { Ok, NotFound, Exhausted, Error };
    QList<EntitlementRecord> GetEntitlements(int64_t userId, int serviceLabel);
    std::optional<EntitlementRecord> GetEntitlement(int64_t userId, const QString& entitlementId,
                                                    int serviceLabel);
    bool GrantEntitlement(int64_t userId, const EntitlementRecord& rec);
    // Consume `count` uses: use_limit -= count, use_count += count (their sum, the
    // original max, is invariant). Fails with Exhausted if count > current use_limit.
    ConsumeResult ConsumeEntitlement(int64_t userId, const QString& entitlementId,
                                     int serviceLabel, int count, EntitlementRecord* out = nullptr);

    // Grant `count` additional uses (use_limit += count; use_count unchanged). Creates
    // the row as a service consumable if absent. For full metadata/durable seeding use
    // GrantEntitlement. Not a webapi op: grants originate from purchases/NPMT.
    bool AddCounts(int64_t userId, const QString& entitlementId, int serviceLabel, int count,
                   EntitlementRecord* out = nullptr);
    // Revoke `count` uses (use_limit -= count, clamped at 0; use_count unchanged), e.g.
    // a refund. Returns false if the entitlement does not exist.
    bool RevokeCounts(int64_t userId, const QString& entitlementId, int serviceLabel, int count,
                      EntitlementRecord* out = nullptr);

    // Products within a commerce container (category), for the container webapi.
    // All products for a service label; the container route filters by the requested
    // colon-separated label list (matching product label or category label).
    QList<ProductRecord> GetAllProducts(int serviceLabel);

    // Single product by label (for checkout grant lookup).
    std::optional<ProductRecord> GetProduct(int serviceLabel, const QString& label);

    // In-game presence (gameStatus). Stored per user + service label; surfaced to
    // friends later. SetPresence upserts; ClearPresence removes (presence DELETE).
    bool SetPresence(int64_t userId, int serviceLabel, const QString& gameStatus,
                     const QString& gameData);
    bool ClearPresence(int64_t userId, int serviceLabel);

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