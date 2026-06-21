// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>
#include <qcryptographichash.h>
#include "database.h"

QByteArray Database::GenerateSalt(int bytes) {
    QByteArray salt(bytes, Qt::Uninitialized);
    QRandomGenerator* generator = QRandomGenerator::global();

    for (int i = 0; i < bytes; ++i) {
        salt[i] = static_cast<char>(generator->bounded(256));
    }

    return salt;
}
QString Database::GenerateToken(int len) {
    const QString chars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                         "abcdefghijklmnopqrstuvwxyz"
                                         "0123456789");
    QString tok;
    tok.reserve(len);

    QRandomGenerator* generator = QRandomGenerator::global();

    for (int i = 0; i < len; ++i) {
        tok.append(chars.at(generator->bounded(chars.size())));
    }

    return tok;
}

QByteArray Database::HashPassword(const QString& password, const QByteArray& salt) {
    int iterations = 100000;
    if (password.isEmpty() || salt.isEmpty()) {
        return QByteArray();
    }

    QByteArray passwordUtf8 = password.toUtf8();
    QByteArray hash = passwordUtf8 + salt;

    // Apply multiple iterations of SHA-256
    for (int i = 0; i < iterations; ++i) {
        hash = QCryptographicHash::hash(hash, QCryptographicHash::Sha256);
    }

    return hash;
}

Database::Database(const QString& connectionName)
    : m_connName(connectionName.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                          : connectionName) {}

Database::~Database() {
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase{};                    // release the reference first
    QSqlDatabase::removeDatabase(m_connName); // now safe to remove
}

bool Database::IsOpen() const {
    return m_db.isOpen();
}

bool Database::Open(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA foreign_keys=ON");
    return Migrate();
}

bool Database::Exec(const QString& sql) {
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}
bool Database::Exec(QSqlQuery& q) {
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::Migrate() {
    Exec("CREATE TABLE IF NOT EXISTS migration("
         "  migration_id UNSIGNED INTEGER PRIMARY KEY,"
         "  description  TEXT NOT NULL)");

    // Migration 1: core tables
    QStringList stmts1 = {
        "CREATE TABLE IF NOT EXISTS account("
        "  user_id     INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username    TEXT NOT NULL,"
        "  hash        BLOB NOT NULL,"
        "  salt        BLOB NOT NULL,"
        "  avatar_url  TEXT NOT NULL,"
        "  email       TEXT NOT NULL,"
        "  email_check TEXT NOT NULL UNIQUE,"
        "  token       TEXT NOT NULL,"
        "  reset_token TEXT,"
        "  admin       BOOL NOT NULL,"
        "  stat_agent  BOOL NOT NULL,"
        "  banned      BOOL NOT NULL,"
        "  UNIQUE(username COLLATE NOCASE))",

        "CREATE TABLE IF NOT EXISTS account_timestamp("
        "  user_id          UNSIGNED BIGINT NOT NULL PRIMARY KEY,"
        "  creation         UNSIGNED INTEGER NOT NULL,"
        "  last_login       UNSIGNED INTEGER,"
        "  token_last_sent  UNSIGNED INTEGER,"
        "  reset_emit       UNSIGNED INTEGER)",

        // Friendship table.
        // user_id_1 < user_id_2 is enforced by CHECK so there is exactly one row
        // per pair regardless of who initiated. status_user_1 and status_user_2
        // each hold a bitmask of FriendStatus flags for their respective user.
        "CREATE TABLE IF NOT EXISTS friendship("
        "  user_id_1     INTEGER NOT NULL REFERENCES account(user_id) ON DELETE CASCADE,"
        "  user_id_2     INTEGER NOT NULL REFERENCES account(user_id) ON DELETE CASCADE,"
        "  status_user_1 INTEGER NOT NULL DEFAULT 0,"
        "  status_user_2 INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(user_id_1, user_id_2),"
        "  CHECK(user_id_1 < user_id_2))",

        "CREATE INDEX IF NOT EXISTS friendship_user1 ON friendship(user_id_1)",
        "CREATE INDEX IF NOT EXISTS friendship_user2 ON friendship(user_id_2)",

        // Score leaderboard configuration it has one row per (comId, boardId) pair.
        "CREATE TABLE IF NOT EXISTS score_table("
        "  communication_id  TEXT    NOT NULL,"
        "  board_id          INTEGER NOT NULL,"
        "  rank_limit        INTEGER NOT NULL DEFAULT 100,"
        "  update_mode       INTEGER NOT NULL DEFAULT 0,"
        "  sort_mode         INTEGER NOT NULL DEFAULT 0,"
        "  upload_num_limit  INTEGER NOT NULL DEFAULT 10,"
        "  upload_size_limit INTEGER NOT NULL DEFAULT 6000000,"
        "  PRIMARY KEY(communication_id, board_id))",

        // Score rows it has one per (comId, boardId, userId, characterId).
        "CREATE TABLE IF NOT EXISTS score("
        "  communication_id TEXT    NOT NULL,"
        "  board_id         INTEGER NOT NULL,"
        "  user_id          INTEGER NOT NULL,"
        "  character_id     INTEGER NOT NULL,"
        "  score            INTEGER NOT NULL,"
        "  comment          TEXT,"
        "  game_info        BLOB,"
        "  data_id          INTEGER,"
        "  timestamp        INTEGER NOT NULL,"
        "  PRIMARY KEY(communication_id, board_id, user_id, character_id))",

        // Owned entitlements, one row per (user, entitlement_id). communication_id
        // groups by owning title for multi-title use; entitlement_id is globally
        // unique (reverse-DNS) so a per-account query spans all titles.
        "CREATE TABLE IF NOT EXISTS entitlement("
        "  communication_id TEXT    NOT NULL DEFAULT '',"
        "  service_label    INTEGER NOT NULL DEFAULT 0,"
        "  user_id          INTEGER NOT NULL REFERENCES account(user_id) ON DELETE CASCADE,"
        "  entitlement_id   TEXT    NOT NULL,"
        "  entitlement_type TEXT    NOT NULL DEFAULT 'service',"
        "  is_consumable    INTEGER NOT NULL DEFAULT 0,"
        "  use_limit        INTEGER NOT NULL DEFAULT 0,"
        "  use_count        INTEGER NOT NULL DEFAULT 0,"
        "  active_date      TEXT    NOT NULL DEFAULT '2024-01-01T00:00:00Z',"
        "  inactive_date    TEXT    NOT NULL DEFAULT '',"  // empty = no expiry
        "  PRIMARY KEY(user_id, service_label, entitlement_id))",

        "CREATE INDEX IF NOT EXISTS entitlement_comm ON entitlement(communication_id)",

        // Commerce container products (reverse-engineered schema; see ProductRecord).
        "CREATE TABLE IF NOT EXISTS product("
        "  communication_id TEXT    NOT NULL DEFAULT '',"
        "  service_label    INTEGER NOT NULL DEFAULT 0,"
        "  container_label  TEXT    NOT NULL,"
        "  label            TEXT    NOT NULL,"
        "  name             TEXT    NOT NULL DEFAULT '',"
        "  long_desc        TEXT    NOT NULL DEFAULT '',"
        "  provider_name    TEXT    NOT NULL DEFAULT '',"
        "  price            INTEGER NOT NULL DEFAULT 0,"
        "  original_price   INTEGER NOT NULL DEFAULT 0,"
        "  display_price    TEXT    NOT NULL DEFAULT '',"
        "  display_original_price TEXT NOT NULL DEFAULT '',"
        "  use_count        INTEGER NOT NULL DEFAULT 0,"
        "  entitlement_id   TEXT    NOT NULL DEFAULT '',"
        "  PRIMARY KEY(service_label, container_label, label))",

        "CREATE INDEX IF NOT EXISTS product_comm ON product(communication_id)",

        // In-game presence (gameStatus) per user + service label.
        "CREATE TABLE IF NOT EXISTS presence("
        "  user_id          INTEGER NOT NULL,"
        "  communication_id TEXT    NOT NULL DEFAULT '',"
        "  service_label    INTEGER NOT NULL DEFAULT 0,"
        "  game_status      TEXT    NOT NULL DEFAULT '',"
        "  game_data        TEXT    NOT NULL DEFAULT '',"
        "  updated_at       TEXT    NOT NULL DEFAULT '',"
        "  PRIMARY KEY(user_id, service_label))",
    };

    for (const QString& s : stmts1)
        Exec(s);

    QSqlQuery ins(m_db);
    ins.prepare("INSERT OR IGNORE INTO migration VALUES(1,'Initial setup')");
    Exec(ins);

    qInfo() << "Database migrations complete";
    return true;
}

std::optional<DbError> Database::CreateAccount(const QString& npid, const QString& password,
                                               const QString& avatarUrl, const QString& email) {
    // Input validation
    if (npid.isEmpty()) {
        qWarning() << "createAccount: NPID is empty";
        return DbError::InvalidInput;
    }

    if (password.isEmpty()) {
        qWarning() << "createAccount: Password is empty";
        return DbError::InvalidInput;
    }

    // Check database connection
    if (!m_db.isOpen() || !m_db.isValid()) {
        qCritical() << "createAccount: Database connection is not valid";
        return DbError::Internal;
    }

    // Username collision check
    {
        QSqlQuery q(m_db);
        if (!q.prepare("SELECT COUNT(*) FROM account WHERE username=? COLLATE NOCASE")) {
            qCritical() << "createAccount: Failed to prepare username check query:"
                        << q.lastError().text();
            return DbError::Internal;
        }

        q.addBindValue(npid);

        if (!q.exec()) {
            qCritical() << "createAccount: Failed to execute username check:"
                        << q.lastError().text();
            return DbError::Internal;
        }

        if (!q.next()) {
            qCritical() << "createAccount: Failed to get username check result";
            return DbError::Internal;
        }

        if (q.value(0).toInt() > 0) {
            qWarning() << "createAccount: Username already exists:" << npid;
            return DbError::ExistingUsername;
        }
    }

    // Email collision check (if email is provided)
    if (!email.isEmpty()) {
        QString emailCheck = email.toLower().trimmed();

        // Basic email format validation
        if (!emailCheck.contains('@') || !emailCheck.contains('.')) {
            qWarning() << "createAccount: Invalid email format:" << email;
            return DbError::InvalidEmail;
        }

        QSqlQuery q(m_db);
        if (!q.prepare("SELECT COUNT(*) FROM account WHERE email_check=?")) {
            qCritical() << "createAccount: Failed to prepare email check query:"
                        << q.lastError().text();
            return DbError::Internal;
        }

        q.addBindValue(emailCheck);

        if (!q.exec()) {
            qCritical() << "createAccount: Failed to execute email check:" << q.lastError().text();
            return DbError::Internal;
        }

        if (!q.next()) {
            qCritical() << "createAccount: Failed to get email check result";
            return DbError::Internal;
        }

        if (q.value(0).toInt() > 0) {
            qWarning() << "createAccount: Email already exists:" << emailCheck;
            return DbError::ExistingEmail;
        }
    }

    // Generate cryptographic values
    QByteArray salt = GenerateSalt();
    if (salt.isEmpty()) {
        qCritical() << "createAccount: Failed to generate salt";
        return DbError::Internal;
    }

    QByteArray hash = HashPassword(password, salt);
    if (hash.isEmpty()) {
        qCritical() << "createAccount: Failed to generate password hash";
        return DbError::Internal;
    }

    QString token = GenerateToken();
    if (token.isEmpty()) {
        qCritical() << "createAccount: Failed to generate token";
        return DbError::Internal;
    }

    qint64 now = QDateTime::currentSecsSinceEpoch();

    // Store the new account ID for later use
    int64_t newId = -1;

    // Start transaction
    if (!m_db.transaction()) {
        qCritical() << "createAccount: Failed to start transaction:" << m_db.lastError().text();
        return DbError::Internal;
    }

    // Insert account
    {
        QSqlQuery q(m_db);
        if (!q.prepare("INSERT INTO account(username, hash, salt, avatar_url, "
                       "email, email_check, token, admin, stat_agent, banned) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?, 0, 0, 0)")) {
            qCritical() << "createAccount: Failed to prepare insert query:" << q.lastError().text();
            m_db.rollback();
            return DbError::Internal;
        }

        q.addBindValue(npid);
        q.addBindValue(hash);
        q.addBindValue(salt);
        q.addBindValue(avatarUrl);
        q.addBindValue(email);
        q.addBindValue(email.isEmpty() ? "" : email.toLower().trimmed());
        q.addBindValue(token);

        if (!q.exec()) {
            qCritical() << "createAccount: Failed to insert account:" << q.lastError().text();
            m_db.rollback();

            // Check for specific SQL errors
            if (q.lastError().nativeErrorCode() == "19" || // SQLITE_CONSTRAINT
                q.lastError().text().contains("UNIQUE", Qt::CaseInsensitive)) {
                return DbError::ExistingUsername;
            }
            return DbError::Internal;
        }

        // Get the new account ID
        QVariant lastId = q.lastInsertId();
        if (!lastId.isValid() || lastId.isNull()) {
            qCritical() << "createAccount: Failed to get last insert ID";
            m_db.rollback();
            return DbError::Internal;
        }

        bool ok;
        newId = lastId.toLongLong(&ok);
        if (!ok || newId <= 0) {
            qCritical() << "createAccount: Invalid last insert ID:" << lastId;
            m_db.rollback();
            return DbError::Internal;
        }
    }

    // Insert timestamp (using a separate query with the newId we saved)
    {
        QSqlQuery q2(m_db);
        if (!q2.prepare("INSERT INTO account_timestamp(user_id, creation) VALUES(?, ?)")) {
            qCritical() << "createAccount: Failed to prepare timestamp query:"
                        << q2.lastError().text();
            m_db.rollback();
            return DbError::Internal;
        }

        q2.addBindValue(static_cast<qlonglong>(newId));
        q2.addBindValue(now);

        if (!q2.exec()) {
            qCritical() << "createAccount: Failed to insert timestamp:" << q2.lastError().text();
            m_db.rollback();
            return DbError::Internal;
        }
    }

    // Commit transaction
    if (!m_db.commit()) {
        qCritical() << "createAccount: Failed to commit transaction:" << m_db.lastError().text();
        m_db.rollback();
        return DbError::Internal;
    }

    qInfo() << "createAccount: Successfully created account:" << npid << "(ID:" << newId << ")";

    return std::nullopt; // success
}

std::optional<UserRecord> Database::CheckUser(const QString& npid, const QString& password,
                                              const QString& token, bool checkToken) {
    QSqlQuery q(m_db);
    q.prepare("SELECT user_id,username,hash,salt,avatar_url,email,email_check,"
              "token,admin,stat_agent,banned FROM account WHERE username=? COLLATE NOCASE");
    q.addBindValue(npid);
    if (!Exec(q) || !q.next())
        return std::nullopt; // Empty = no such user

    const QString canonicalUsername = q.value(1).toString();
    if (canonicalUsername != npid) {
        return std::nullopt;
    }

    UserRecord r;
    r.userId = q.value(0).toLongLong();
    r.username = canonicalUsername; // exact match
    r.hash = q.value(2).toByteArray();
    r.salt = q.value(3).toByteArray();
    r.avatarUrl = q.value(4).toString();
    r.email = q.value(5).toString();
    r.emailCheck = q.value(6).toString();
    r.token = q.value(7).toString();
    r.admin = q.value(8).toBool();
    r.statAgent = q.value(9).toBool();
    r.banned = q.value(10).toBool();

    QByteArray computed = HashPassword(password, r.salt);
    if (computed != r.hash)
        return std::nullopt; // WrongPass

    if (checkToken && r.token != token)
        return std::nullopt; // WrongToken

    return r;
}

std::optional<int64_t> Database::GetUserId(const QString& npid) {
    QSqlQuery q(m_db);
    q.prepare("SELECT user_id FROM account WHERE username=? COLLATE NOCASE");
    q.addBindValue(npid);
    if (!Exec(q) || !q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

std::optional<QString> Database::GetUsername(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT username FROM account WHERE user_id=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q) || !q.next())
        return std::nullopt;
    return q.value(0).toString();
}

QList<QPair<int64_t, QString>> Database::GetUsernamesFromIds(const QSet<int64_t>& ids) {
    QList<QPair<int64_t, QString>> result;
    if (ids.isEmpty())
        return result;

    // Build IN clause
    QStringList placeholders;
    for (int i = 0; i < ids.size(); ++i)
        placeholders << "?";
    QSqlQuery q(m_db);
    q.prepare(QString("SELECT user_id,username FROM account WHERE user_id IN (%1)")
                  .arg(placeholders.join(',')));
    for (int64_t id : ids)
        q.addBindValue(static_cast<qlonglong>(id));
    if (!Exec(q))
        return result;
    while (q.next())
        result << qMakePair(q.value(0).toLongLong(), q.value(1).toString());
    return result;
}

bool Database::UpdateLoginTime(int64_t userId) {
    uint64_t now = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
    QSqlQuery q(m_db);
    q.prepare("UPDATE account_timestamp SET last_login=? WHERE user_id=?");
    q.addBindValue(static_cast<qint64>(now));
    q.addBindValue(static_cast<qlonglong>(userId));
    return Exec(q);
}

bool Database::BanUser(int64_t userId, bool ban) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET banned=? WHERE user_id=?");
    q.addBindValue(ban ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(userId));
    return Exec(q);
}

bool Database::DeleteUser(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM account WHERE user_id=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    return Exec(q);
}

bool Database::SetAdmin(int64_t userId, bool admin) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET admin=? WHERE user_id=?");
    q.addBindValue(admin ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(userId));
    return Exec(q);
}

int Database::TotalUsers() {
    QSqlQuery q(m_db);
    q.exec("SELECT COUNT(*) FROM account");
    return q.next() ? q.value(0).toInt() : 0;
}

void Database::CleanNeverUsedAccounts() {
    // Delete accounts that never logged in and are older than 30 days
    uint64_t cutoff = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch()) - 30 * 86400;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM account WHERE user_id IN ("
              "  SELECT user_id FROM account_timestamp WHERE creation < ? AND last_login IS NULL)");
    q.addBindValue(static_cast<qint64>(cutoff));
    Exec(q);
}

// Friendship DB methods

// The friendship table always stores rows with user_id_1 < user_id_2.
// This helper returns the canonical (lower, higher) order and a flag
// indicating whether the caller is user_id_2 (i.e. the IDs were swapped).
static std::tuple<int64_t, int64_t, bool> orderedIds(int64_t a, int64_t b) {
    if (a < b)
        return {a, b, false};
    return {b, a, true};
}

std::pair<Database::RelResult, Database::RelStatus> Database::GetRelStatus(int64_t callerId,
                                                                           int64_t otherId) {
    auto [id1, id2, swapped] = orderedIds(callerId, otherId);
    QSqlQuery q(m_db);
    q.prepare("SELECT status_user_1, status_user_2 FROM friendship "
              "WHERE user_id_1=? AND user_id_2=?");
    q.addBindValue(static_cast<qlonglong>(id1));
    q.addBindValue(static_cast<qlonglong>(id2));
    if (!Exec(q))
        return {RelResult::Error, {}};
    if (!q.next())
        return {RelResult::Empty, {}};

    RelStatus s;
    uint8_t s1 = static_cast<uint8_t>(q.value(0).toInt());
    uint8_t s2 = static_cast<uint8_t>(q.value(1).toInt());
    s.caller = swapped ? s2 : s1;
    s.other = swapped ? s1 : s2;
    return {RelResult::Ok, s};
}

bool Database::SetRelStatus(int64_t callerId, int64_t otherId, uint8_t statusCaller,
                            uint8_t statusOther) {
    auto [id1, id2, swapped] = orderedIds(callerId, otherId);
    uint8_t s1 = swapped ? statusOther : statusCaller;
    uint8_t s2 = swapped ? statusCaller : statusOther;

    QSqlQuery q(m_db);
    q.prepare("INSERT INTO friendship(user_id_1, user_id_2, status_user_1, status_user_2) "
              "VALUES(?,?,?,?) "
              "ON CONFLICT(user_id_1, user_id_2) DO UPDATE SET "
              "status_user_1=excluded.status_user_1, status_user_2=excluded.status_user_2");
    q.addBindValue(static_cast<qlonglong>(id1));
    q.addBindValue(static_cast<qlonglong>(id2));
    q.addBindValue(s1);
    q.addBindValue(s2);
    return Exec(q);
}

bool Database::DeleteRel(int64_t callerId, int64_t otherId) {
    auto [id1, id2, swapped] = orderedIds(callerId, otherId);
    Q_UNUSED(swapped);
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM friendship WHERE user_id_1=? AND user_id_2=?");
    q.addBindValue(static_cast<qlonglong>(id1));
    q.addBindValue(static_cast<qlonglong>(id2));
    return Exec(q);
}

UserRelationships Database::GetRelationships(int64_t userId) {
    UserRelationships result;

    QSqlQuery q(m_db);
    q.prepare("SELECT user_id_1, user_id_2, status_user_1, status_user_2 "
              "FROM friendship WHERE user_id_1=? OR user_id_2=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return result;

    constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
    constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);

    while (q.next()) {
        int64_t uid1 = q.value(0).toLongLong();
        int64_t uid2 = q.value(1).toLongLong();
        uint8_t su1 = static_cast<uint8_t>(q.value(2).toInt());
        uint8_t su2 = static_cast<uint8_t>(q.value(3).toInt());

        // Rotate so statusMe / statusOther are from our perspective.
        int64_t otherId;
        uint8_t statusMe, statusOther;
        if (uid1 == userId) {
            otherId = uid2;
            statusMe = su1;
            statusOther = su2;
        } else {
            otherId = uid1;
            statusMe = su2;
            statusOther = su1;
        }

        // Resolve the other user's npid.
        auto npidOpt = GetUsername(otherId);
        if (!npidOpt)
            continue;
        auto pair = qMakePair(otherId, *npidOpt);

        if ((statusMe & F) && (statusOther & F)) {
            result.friends.append(pair);
        } else if ((statusMe & F) && !(statusOther & F)) {
            result.friendRequestsSent.append(pair);
        } else if (!(statusMe & F) && (statusOther & F)) {
            result.friendRequestsReceived.append(pair);
        }
        // Blocked: we blocked them
        if (statusMe & B) {
            result.blocked.append(pair);
        }
    }

    return result;
}

// ── Entitlements ──────────────────────────────────────────────────────────────
QList<EntitlementRecord> Database::GetEntitlements(int64_t userId, int serviceLabel) {
    QList<EntitlementRecord> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT communication_id, entitlement_id, entitlement_type, is_consumable, "
              "use_limit, use_count, active_date, inactive_date, service_label "
              "FROM entitlement WHERE user_id=? AND service_label=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(serviceLabel);
    if (!Exec(q))
        return out;
    while (q.next()) {
        EntitlementRecord r;
        r.communicationId = q.value(0).toString();
        r.entitlementId = q.value(1).toString();
        r.entitlementType = q.value(2).toString();
        r.isConsumable = q.value(3).toBool();
        r.useLimit = q.value(4).toLongLong();
        r.useCount = q.value(5).toLongLong();
        r.activeDate = q.value(6).toString();
        r.inactiveDate = q.value(7).toString();
        r.serviceLabel = q.value(8).toInt();
        out.append(r);
    }
    return out;
}

std::optional<EntitlementRecord> Database::GetEntitlement(int64_t userId,
                                                          const QString& entitlementId,
                                                          int serviceLabel) {
    QSqlQuery q(m_db);
    q.prepare("SELECT communication_id, entitlement_id, entitlement_type, is_consumable, "
              "use_limit, use_count, active_date, inactive_date, service_label "
              "FROM entitlement WHERE user_id=? AND entitlement_id=? AND service_label=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(entitlementId);
    q.addBindValue(serviceLabel);
    if (!Exec(q) || !q.next())
        return std::nullopt;
    EntitlementRecord r;
    r.communicationId = q.value(0).toString();
    r.entitlementId = q.value(1).toString();
    r.entitlementType = q.value(2).toString();
    r.isConsumable = q.value(3).toBool();
    r.useLimit = q.value(4).toLongLong();
    r.useCount = q.value(5).toLongLong();
    r.activeDate = q.value(6).toString();
    r.inactiveDate = q.value(7).toString();
    r.serviceLabel = q.value(8).toInt();
    return r;
}

bool Database::GrantEntitlement(int64_t userId, const EntitlementRecord& rec) {
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO entitlement(communication_id, service_label, user_id, "
              "entitlement_id, entitlement_type, is_consumable, use_limit, use_count, "
              "active_date, inactive_date) VALUES(?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(rec.communicationId);
    q.addBindValue(rec.serviceLabel);
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(rec.entitlementId);
    q.addBindValue(rec.entitlementType);
    q.addBindValue(rec.isConsumable);
    q.addBindValue(static_cast<qlonglong>(rec.useLimit));
    q.addBindValue(static_cast<qlonglong>(rec.useCount));
    q.addBindValue(rec.activeDate);
    q.addBindValue(rec.inactiveDate);
    return Exec(q);
}

// Consume `count` uses of an entitlement. Per the entitlement webapi consumption spec,
// use_limit holds the REMAINING uses and use_count the consumed uses: one consumption
// decrements use_limit and increments use_count, and their sum (the original maximum) is
// invariant. A durable/unified entitlement has use_limit=0 and therefore cannot be
// consumed. Fails with Exhausted when count exceeds the remaining use_limit.
Database::ConsumeResult Database::ConsumeEntitlement(int64_t userId,
                                                     const QString& entitlementId,
                                                     int serviceLabel, int count,
                                                     EntitlementRecord* out) {
    if (count <= 0)
        return ConsumeResult::Error;
    auto cur = GetEntitlement(userId, entitlementId, serviceLabel);
    if (!cur.has_value())
        return ConsumeResult::NotFound;
    if (cur->useLimit < count)
        return ConsumeResult::Exhausted;

    QSqlQuery q(m_db);
    q.prepare("UPDATE entitlement SET use_limit = use_limit - ?, use_count = use_count + ? "
              "WHERE user_id=? AND entitlement_id=? AND service_label=?");
    q.addBindValue(count);
    q.addBindValue(count);
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(entitlementId);
    q.addBindValue(serviceLabel);
    if (!Exec(q))
        return ConsumeResult::Error;

    cur->useLimit -= count;
    cur->useCount += count;
    if (out)
        *out = *cur;
    return ConsumeResult::Ok;
}

// Grant additional uses. use_count is left unchanged so the sum (use_limit + use_count)
// rises by `count`, marking a grant rather than a use (see Tracking Entitlement Changes).
bool Database::AddCounts(int64_t userId, const QString& entitlementId, int serviceLabel,
                         int count, EntitlementRecord* out) {
    if (count <= 0)
        return false;
    auto cur = GetEntitlement(userId, entitlementId, serviceLabel);
    if (cur.has_value()) {
        QSqlQuery q(m_db);
        q.prepare("UPDATE entitlement SET use_limit = use_limit + ? "
                  "WHERE user_id=? AND entitlement_id=? AND service_label=?");
        q.addBindValue(count);
        q.addBindValue(static_cast<qlonglong>(userId));
        q.addBindValue(entitlementId);
        q.addBindValue(serviceLabel);
        if (!Exec(q))
            return false;
        cur->useLimit += count;
        if (out)
            *out = *cur;
        return true;
    }
    // Absent: create as a service consumable carrying `count` uses.
    EntitlementRecord rec;
    rec.entitlementId = entitlementId;
    rec.entitlementType = QStringLiteral("service");
    rec.isConsumable = true;
    rec.serviceLabel = serviceLabel;
    rec.useLimit = count;
    rec.useCount = 0;
    if (!GrantEntitlement(userId, rec))
        return false;
    if (out)
        *out = rec;
    return true;
}

// Revoke uses (e.g. a refund): use_limit drops by `count` (clamped at 0) while use_count
// stays the same, so the sum decreases -- distinguishing a revocation from a consumption.
bool Database::RevokeCounts(int64_t userId, const QString& entitlementId, int serviceLabel,
                            int count, EntitlementRecord* out) {
    if (count <= 0)
        return false;
    auto cur = GetEntitlement(userId, entitlementId, serviceLabel);
    if (!cur.has_value())
        return false;
    int64_t newLimit = cur->useLimit - count;
    if (newLimit < 0)
        newLimit = 0;
    QSqlQuery q(m_db);
    q.prepare("UPDATE entitlement SET use_limit = ? "
              "WHERE user_id=? AND entitlement_id=? AND service_label=?");
    q.addBindValue(static_cast<qlonglong>(newLimit));
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(entitlementId);
    q.addBindValue(serviceLabel);
    if (!Exec(q))
        return false;
    cur->useLimit = newLimit;
    if (out)
        *out = *cur;
    return true;
}

// Products within a commerce container (category). Ordered by label for stable output.
QList<ProductRecord> Database::GetAllProducts(int serviceLabel) {
    QList<ProductRecord> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT communication_id, service_label, container_label, label, name, "
              "long_desc, provider_name, price, original_price, display_price, "
              "display_original_price, use_count, entitlement_id "
              "FROM product WHERE service_label=? ORDER BY container_label, label");
    q.addBindValue(serviceLabel);
    if (!Exec(q))
        return out;
    while (q.next()) {
        ProductRecord r;
        r.communicationId = q.value(0).toString();
        r.serviceLabel = q.value(1).toInt();
        r.containerLabel = q.value(2).toString();
        r.label = q.value(3).toString();
        r.name = q.value(4).toString();
        r.longDesc = q.value(5).toString();
        r.providerName = q.value(6).toString();
        r.price = q.value(7).toLongLong();
        r.originalPrice = q.value(8).toLongLong();
        r.displayPrice = q.value(9).toString();
        r.displayOriginalPrice = q.value(10).toString();
        r.useCount = q.value(11).toLongLong();
        r.entitlementId = q.value(12).toString();
        out.append(r);
    }
    return out;
}

std::optional<ProductRecord> Database::GetProduct(int serviceLabel, const QString& label) {
    QSqlQuery q(m_db);
    q.prepare("SELECT communication_id, service_label, container_label, label, name, "
              "long_desc, provider_name, price, original_price, display_price, "
              "display_original_price, use_count, entitlement_id "
              "FROM product WHERE service_label=? AND label=? LIMIT 1");
    q.addBindValue(serviceLabel);
    q.addBindValue(label);
    if (!Exec(q) || !q.next())
        return std::nullopt;
    ProductRecord r;
    r.communicationId = q.value(0).toString();
    r.serviceLabel = q.value(1).toInt();
    r.containerLabel = q.value(2).toString();
    r.label = q.value(3).toString();
    r.name = q.value(4).toString();
    r.longDesc = q.value(5).toString();
    r.providerName = q.value(6).toString();
    r.price = q.value(7).toLongLong();
    r.originalPrice = q.value(8).toLongLong();
    r.displayPrice = q.value(9).toString();
    r.displayOriginalPrice = q.value(10).toString();
    r.useCount = q.value(11).toLongLong();
    r.entitlementId = q.value(12).toString();
    return r;
}

bool Database::SetPresence(int64_t userId, int serviceLabel, const QString& gameStatus,
                           const QString& gameData) {
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO presence(user_id, service_label, game_status, "
              "game_data, updated_at) VALUES(?, ?, ?, ?, ?)");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(serviceLabel);
    q.addBindValue(gameStatus);
    q.addBindValue(gameData);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return Exec(q);
}

bool Database::ClearPresence(int64_t userId, int serviceLabel) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM presence WHERE user_id=? AND service_label=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(serviceLabel);
    return Exec(q);
}
