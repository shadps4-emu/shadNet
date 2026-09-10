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
    Exec("PRAGMA busy_timeout=5000");
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

bool Database::HasMigration(int id) {
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM migration WHERE migration_id=?");
    q.addBindValue(id);
    return Exec(q) && q.next() && q.value(0).toInt() > 0;
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

        // TUS variable rows: one per (comId, owner, slot).
        "CREATE TABLE IF NOT EXISTS tus_variable("
        "  communication_id       TEXT    NOT NULL,"
        "  owner_user_id          INTEGER NOT NULL,"
        "  slot_id                INTEGER NOT NULL,"
        "  variable               INTEGER NOT NULL DEFAULT 0,"
        "  last_changed           INTEGER NOT NULL,"
        "  last_changed_author_id INTEGER NOT NULL,"
        "  PRIMARY KEY(communication_id, owner_user_id, slot_id))",

        // TUS data slots: payload + small info blob, one per (comId, owner, slot).
        "CREATE TABLE IF NOT EXISTS tus_data("
        "  communication_id       TEXT    NOT NULL,"
        "  owner_user_id          INTEGER NOT NULL,"
        "  slot_id                INTEGER NOT NULL,"
        "  data                   BLOB,"
        "  info                   BLOB,"
        "  data_size              INTEGER NOT NULL DEFAULT 0,"
        "  last_changed           INTEGER NOT NULL,"
        "  last_changed_author_id INTEGER NOT NULL,"
        "  PRIMARY KEY(communication_id, owner_user_id, slot_id))",

        // TUS variables owned by a virtual user
        "CREATE TABLE IF NOT EXISTS tus_vuser_variable("
        "  communication_id       TEXT    NOT NULL,"
        "  virtual_user           TEXT    NOT NULL,"
        "  slot_id                INTEGER NOT NULL,"
        "  variable               INTEGER NOT NULL DEFAULT 0,"
        "  last_changed           INTEGER NOT NULL,"
        "  last_changed_author_id INTEGER NOT NULL,"
        "  PRIMARY KEY(communication_id, virtual_user, slot_id))",

        // TUS data owned by a virtual user
        "CREATE TABLE IF NOT EXISTS tus_vuser_data("
        "  communication_id       TEXT    NOT NULL,"
        "  virtual_user           TEXT    NOT NULL,"
        "  slot_id                INTEGER NOT NULL,"
        "  data                   BLOB,"
        "  info                   BLOB,"
        "  data_size              INTEGER NOT NULL DEFAULT 0,"
        "  last_changed           INTEGER NOT NULL,"
        "  last_changed_author_id INTEGER NOT NULL,"
        "  PRIMARY KEY(communication_id, virtual_user, slot_id))",
    };

    for (const QString& s : stmts1)
        Exec(s);

    QSqlQuery ins(m_db);
    ins.prepare("INSERT OR IGNORE INTO migration VALUES(1,'Initial setup')");
    Exec(ins);

    QStringList stmts2 = {
        "CREATE TABLE IF NOT EXISTS title_name("
        "  communication_id TEXT NOT NULL PRIMARY KEY,"
        "  title_name       TEXT NOT NULL)",
    };

    for (const QString& s : stmts2)
        Exec(s);

    QSqlQuery ins2(m_db);
    ins2.prepare("INSERT OR IGNORE INTO migration VALUES(2,'title_name mapping')");
    Exec(ins2);

    if (!HasMigration(3)) {
        Exec("ALTER TABLE account ADD COLUMN ban_reason TEXT");
        Exec("ALTER TABLE account ADD COLUMN ban_timestamp INTEGER");

        // Append-only record of every privileged action taken through the admin API.
        Exec("CREATE TABLE IF NOT EXISTS admin_audit("
             "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  timestamp      INTEGER NOT NULL,"
             "  actor_user_id  INTEGER NOT NULL,"
             "  actor_npid     TEXT    NOT NULL,"
             "  action         TEXT    NOT NULL,"
             "  target_user_id INTEGER,"
             "  target_npid    TEXT,"
             "  reason         TEXT)");
        Exec("CREATE INDEX IF NOT EXISTS admin_audit_time ON admin_audit(timestamp DESC)");

        QSqlQuery ins3(m_db);
        ins3.prepare("INSERT OR IGNORE INTO migration VALUES(3,'admin tooling: ban metadata + "
                     "audit log')");
        Exec(ins3);
    }

    // Migration 4: per-account trophy records.
    if (!HasMigration(4)) {
        Exec("CREATE TABLE IF NOT EXISTS user_trophies("
             "  user_id          UNSIGNED BIGINT NOT NULL,"
             "  communication_id TEXT            NOT NULL,"
             "  trophy_id        INTEGER         NOT NULL,"
             "  earned_at        UNSIGNED BIGINT NOT NULL,"
             "  PRIMARY KEY(user_id, communication_id, trophy_id),"
             "  FOREIGN KEY(user_id) REFERENCES account(user_id) ON DELETE CASCADE)");
        // Earned-percentage and per-game stats scan by com id, not by user.
        Exec("CREATE INDEX IF NOT EXISTS user_trophies_game "
             "ON user_trophies(communication_id, trophy_id)");

        QSqlQuery ins4(m_db);
        ins4.prepare("INSERT OR IGNORE INTO migration VALUES(4,'user trophy records')");
        Exec(ins4);
    }

    // Migration 5: trophy metadata, imported by an operator from a title's
    // TROP.XML.
    if (!HasMigration(5)) {
        Exec("CREATE TABLE IF NOT EXISTS trophy_meta("
             "  communication_id TEXT    NOT NULL,"
             "  trophy_id        INTEGER NOT NULL,"
             "  name             TEXT    NOT NULL,"
             "  detail           TEXT,"
             "  grade            TEXT," // B, S, G or P
             "  hidden           BOOL    NOT NULL DEFAULT 0,"
             "  group_id         INTEGER NOT NULL DEFAULT 0,"
             "  language         TEXT," // which TROP_xx.XML it came from
             "  imported_at      UNSIGNED BIGINT NOT NULL,"
             "  PRIMARY KEY(communication_id, trophy_id))");

        QSqlQuery ins5(m_db);
        ins5.prepare("INSERT OR IGNORE INTO migration VALUES(5,'trophy metadata')");
        Exec(ins5);
    }

    // Migration 6
    if (!HasMigration(6)) {
        Exec("ALTER TABLE account ADD COLUMN client_version TEXT");
        Exec("ALTER TABLE account ADD COLUMN client_version_at UNSIGNED BIGINT");

        QSqlQuery ins6(m_db);
        ins6.prepare("INSERT OR IGNORE INTO migration VALUES(6,'client version')");
        Exec(ins6);
    }

    // Migration 7: trophy group names, so DLC packs can be shown as their own
    // sections rather than mixed into the base game's list. Comes from the same
    // TROP.XML as the trophy names.
    if (!HasMigration(7)) {
        Exec("CREATE TABLE IF NOT EXISTS trophy_group("
             "  communication_id TEXT    NOT NULL,"
             "  group_id         INTEGER NOT NULL,"
             "  name             TEXT    NOT NULL,"
             "  detail           TEXT,"
             "  PRIMARY KEY(communication_id, group_id))");

        QSqlQuery ins7(m_db);
        ins7.prepare("INSERT OR IGNORE INTO migration VALUES(7,'trophy groups')");
        Exec(ins7);
    }

    qInfo() << "Database migrations complete";

    RunMaintenance();
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

std::optional<QString> Database::GetAvatarUrl(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT avatar_url FROM account WHERE user_id=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q) || !q.next())
        return std::nullopt;
    return q.value(0).toString();
}

std::optional<int64_t> Database::GetAccountCreationTime(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT creation FROM account_timestamp WHERE user_id=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q) || !q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
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

bool Database::BanUser(int64_t userId, bool ban, const QString& reason) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET banned=?, ban_reason=?, ban_timestamp=? WHERE user_id=?");
    q.addBindValue(ban ? 1 : 0);
    // Unbanning clears the reason so a stale note never outlives the ban itself.
    // A default-constructed QVariant binds as SQL NULL.
    q.addBindValue(ban && !reason.isEmpty() ? QVariant(reason) : QVariant());
    q.addBindValue(ban ? QVariant(QDateTime::currentSecsSinceEpoch()) : QVariant());
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

QString Database::BuildUserFilterClause(const QString& search, UserFilter filter,
                                        const std::optional<QList<int64_t>>& restrictToIds) {
    QStringList clauses;
    switch (filter) {
    case UserFilter::BannedOnly:
        clauses << "a.banned = 1";
        break;
    case UserFilter::AdminsOnly:
        clauses << "a.admin = 1";
        break;
    case UserFilter::ActiveOnly:
        clauses << "a.banned = 0";
        break;
    case UserFilter::WithScores:
        clauses << "EXISTS (SELECT 1 FROM score s WHERE s.user_id = a.user_id)";
        break;
    case UserFilter::WithTrophies:
        clauses << "EXISTS (SELECT 1 FROM user_trophies t WHERE t.user_id = a.user_id)";
        break;
    case UserFilter::OnlineOnly:
        break;
    case UserFilter::All:
        break;
    }

    if (!search.isEmpty()) {
        clauses << "(a.username LIKE ? ESCAPE '\\' OR a.email LIKE ? ESCAPE '\\')";
    }
    if (restrictToIds) {
        if (restrictToIds->isEmpty()) {
            clauses << "0";
        } else {
            QStringList placeholders;
            for (int i = 0; i < restrictToIds->size(); ++i)
                placeholders << QStringLiteral("?");
            clauses << QStringLiteral("a.user_id IN (%1)").arg(placeholders.join(QLatin1Char(',')));
        }
    }
    return clauses.isEmpty() ? QString() : QStringLiteral(" WHERE ") + clauses.join(" AND ");
}

void Database::BindUserFilter(QSqlQuery& q, const QString& search,
                              const std::optional<QList<int64_t>>& restrictToIds) {
    if (!search.isEmpty()) {
        QString escaped = search;
        escaped.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        const QString pattern = QStringLiteral("%%%1%%").arg(escaped);
        q.addBindValue(pattern);
        q.addBindValue(pattern);
    }
    if (restrictToIds) {
        for (int64_t id : *restrictToIds)
            q.addBindValue(static_cast<qlonglong>(id));
    }
}

QList<AdminUserRow> Database::ListUsers(const QString& search, UserFilter filter, int limit,
                                        int offset,
                                        const std::optional<QList<int64_t>>& restrictToIds) {
    QList<AdminUserRow> rows;
    if (limit <= 0)
        return rows;

    const QString sql =
        QStringLiteral("SELECT a.user_id, a.username, a.email, a.admin, a.stat_agent, a.banned, "
                       "a.ban_reason, a.ban_timestamp, t.creation, t.last_login, "
                       "COALESCE(a.client_version,''), COALESCE(a.client_version_at,0) "
                       "FROM account a LEFT JOIN account_timestamp t ON t.user_id = a.user_id") +
        BuildUserFilterClause(search, filter, restrictToIds) +
        QStringLiteral(" ORDER BY a.user_id ASC LIMIT ? OFFSET ?");

    QSqlQuery q(m_db);
    if (!q.prepare(sql)) {
        m_lastError = q.lastError().text();
        qWarning() << "ListUsers: prepare failed:" << m_lastError;
        return rows;
    }
    BindUserFilter(q, search, restrictToIds);
    q.addBindValue(limit);
    q.addBindValue(qMax(0, offset));
    if (!Exec(q)) {
        qWarning() << "ListUsers: exec failed:" << m_lastError;
        return rows;
    }

    while (q.next()) {
        AdminUserRow r;
        r.userId = q.value(0).toLongLong();
        r.username = q.value(1).toString();
        r.email = q.value(2).toString();
        r.admin = q.value(3).toBool();
        r.statAgent = q.value(4).toBool();
        r.banned = q.value(5).toBool();
        r.banReason = q.value(6).toString();
        r.banTimestamp = q.value(7).toLongLong();
        r.creation = q.value(8).toLongLong();
        r.lastLogin = q.value(9).toLongLong();
        r.clientVersion = q.value(10).toString();
        r.clientVersionAt = q.value(11).toLongLong();
        rows.append(r);
    }
    return rows;
}

int Database::CountUsers(const QString& search, UserFilter filter,
                         const std::optional<QList<int64_t>>& restrictToIds) {
    const QString sql = QStringLiteral("SELECT COUNT(*) FROM account a") +
                        BuildUserFilterClause(search, filter, restrictToIds);
    QSqlQuery q(m_db);
    if (!q.prepare(sql)) {
        m_lastError = q.lastError().text();
        return 0;
    }
    BindUserFilter(q, search, restrictToIds);
    return (Exec(q) && q.next()) ? q.value(0).toInt() : 0;
}

int Database::CountUsersWhere(UserFilter filter) {
    return CountUsers(QString(), filter);
}

std::optional<AdminUserRow> Database::GetUserRow(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT a.user_id, a.username, a.email, a.admin, a.stat_agent, a.banned, "
              "a.ban_reason, a.ban_timestamp, t.creation, t.last_login, "
              "COALESCE(a.client_version,''), COALESCE(a.client_version_at,0) "
              "FROM account a LEFT JOIN account_timestamp t ON t.user_id = a.user_id "
              "WHERE a.user_id = ?");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q) || !q.next())
        return std::nullopt;

    AdminUserRow r;
    r.userId = q.value(0).toLongLong();
    r.username = q.value(1).toString();
    r.email = q.value(2).toString();
    r.admin = q.value(3).toBool();
    r.statAgent = q.value(4).toBool();
    r.banned = q.value(5).toBool();
    r.banReason = q.value(6).toString();
    r.banTimestamp = q.value(7).toLongLong();
    r.creation = q.value(8).toLongLong();
    r.lastLogin = q.value(9).toLongLong();
    r.clientVersion = q.value(10).toString();
    r.clientVersionAt = q.value(11).toLongLong();
    return r;
}

bool Database::AddAuditEntry(int64_t actorUserId, const QString& actorNpid, const QString& action,
                             int64_t targetUserId, const QString& targetNpid,
                             const QString& reason) {
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO admin_audit(timestamp, actor_user_id, actor_npid, action, "
              "target_user_id, target_npid, reason) VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(static_cast<qlonglong>(actorUserId));
    q.addBindValue(actorNpid);
    q.addBindValue(action);
    q.addBindValue(static_cast<qlonglong>(targetUserId));
    q.addBindValue(targetNpid);
    q.addBindValue(reason);
    return Exec(q);
}

QList<AuditRow> Database::ListAudit(int limit, int offset) {
    QList<AuditRow> rows;
    if (limit <= 0)
        return rows;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, timestamp, actor_user_id, actor_npid, action, target_user_id, "
              "target_npid, reason FROM admin_audit ORDER BY id DESC LIMIT ? OFFSET ?");
    q.addBindValue(limit);
    q.addBindValue(qMax(0, offset));
    if (!Exec(q))
        return rows;
    while (q.next()) {
        AuditRow r;
        r.id = q.value(0).toLongLong();
        r.timestamp = q.value(1).toLongLong();
        r.actorUserId = q.value(2).toLongLong();
        r.actorNpid = q.value(3).toString();
        r.action = q.value(4).toString();
        r.targetUserId = q.value(5).toLongLong();
        r.targetNpid = q.value(6).toString();
        r.reason = q.value(7).toString();
        rows.append(r);
    }
    return rows;
}

bool Database::SetAvatarUrl(int64_t userId, const QString& avatarUrl) {
    // avatar_url is NOT NULL, and an account with no picture is not a state the
    // rest of the server expects; callers wanting the default should pass it.
    if (avatarUrl.isEmpty()) {
        m_lastError = QStringLiteral("Avatar URL must not be empty");
        return false;
    }

    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET avatar_url=? WHERE user_id=?");
    q.addBindValue(avatarUrl);
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return false;
    if (q.numRowsAffected() <= 0) {
        m_lastError = QStringLiteral("No such account");
        return false;
    }
    return true;
}

bool Database::SetPassword(int64_t userId, const QString& newPassword) {
    if (newPassword.isEmpty()) {
        m_lastError = QStringLiteral("Password must not be empty");
        return false;
    }

    const QByteArray salt = GenerateSalt();
    if (salt.isEmpty()) {
        m_lastError = QStringLiteral("Failed to generate salt");
        qCritical() << "SetPassword: could not generate salt";
        return false;
    }
    const QByteArray hash = HashPassword(newPassword, salt);
    if (hash.isEmpty()) {
        m_lastError = QStringLiteral("Failed to hash password");
        qCritical() << "SetPassword: could not hash password";
        return false;
    }

    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET hash=?, salt=?, token=?, reset_token=NULL WHERE user_id=?");
    q.addBindValue(hash);
    q.addBindValue(salt);
    q.addBindValue(GenerateToken());
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

bool Database::SetClientVersion(int64_t userId, const QString& version) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET client_version=?, client_version_at=? WHERE user_id=?");
    q.addBindValue(version.isEmpty() ? QVariant() : QVariant(version));
    q.addBindValue(version.isEmpty() ? QVariant() : QVariant(QDateTime::currentSecsSinceEpoch()));
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

bool Database::SetAdmin(int64_t userId, bool admin) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE account SET admin=? WHERE user_id=?");
    q.addBindValue(admin ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(userId));
    return Exec(q);
}

void Database::CollectScoreDataIds(int64_t userId, PurgeSummary& summary) {
    QSqlQuery q(m_db);
    q.prepare("SELECT data_id FROM score WHERE user_id=? AND data_id IS NOT NULL");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return;
    while (q.next()) {
        const qlonglong id = q.value(0).toLongLong();
        if (id > 0)
            summary.scoreDataIds.append(static_cast<uint64_t>(id));
    }
}

bool Database::PurgeUserDataStatements(int64_t userId, PurgeSummary& summary) {
    const qlonglong uid = static_cast<qlonglong>(userId);

    // (sql, bind count, where to record the row count)
    const struct {
        const char* sql;
        int binds;
        int* counter;
    } steps[] = {
        {"DELETE FROM score WHERE user_id=?", 1, &summary.scores},
        {"DELETE FROM user_trophies WHERE user_id=?", 1, &summary.trophies},
        {"DELETE FROM tus_variable WHERE owner_user_id=?", 1, &summary.tusVariables},
        {"DELETE FROM tus_data WHERE owner_user_id=?", 1, &summary.tusData},
        {"DELETE FROM friendship WHERE user_id_1=? OR user_id_2=?", 2, &summary.friendships},
    };

    for (const auto& step : steps) {
        QSqlQuery q(m_db);
        if (!q.prepare(QString::fromLatin1(step.sql))) {
            m_lastError = q.lastError().text();
            qCritical() << "PurgeUserData: prepare failed:" << m_lastError;
            return false;
        }
        for (int i = 0; i < step.binds; ++i)
            q.addBindValue(uid);
        if (!Exec(q)) {
            qCritical() << "PurgeUserData: delete failed:" << m_lastError;
            return false;
        }
        *step.counter = qMax(0, q.numRowsAffected());
    }
    return true;
}

bool Database::PurgeUserData(int64_t userId, PurgeSummary& summary) {
    summary = PurgeSummary{};

    // Collect the score blob ids before the rows go away, so the caller can delete
    // the matching files. Done outside the transaction: it's a read.
    CollectScoreDataIds(userId, summary);

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "PurgeUserData: cannot start transaction:" << m_lastError;
        return false;
    }
    if (!PurgeUserDataStatements(userId, summary)) {
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "PurgeUserData: commit failed:" << m_lastError;
        m_db.rollback();
        return false;
    }

    qInfo() << "PurgeUserData: user" << userId << "— scores:" << summary.scores
            << "trophies:" << summary.trophies << "tus variables:" << summary.tusVariables
            << "tus data:" << summary.tusData << "relationships:" << summary.friendships;
    return true;
}

bool Database::DeleteAccount(int64_t userId, PurgeSummary& summary) {
    summary = PurgeSummary{};
    const qlonglong uid = static_cast<qlonglong>(userId);

    CollectScoreDataIds(userId, summary);

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "DeleteAccount: cannot start transaction:" << m_lastError;
        return false;
    }

    // Same data sweep as a purge, then the account itself.
    if (!PurgeUserDataStatements(userId, summary)) {
        m_db.rollback();
        return false;
    }

    for (const char* sql :
         {"DELETE FROM account_timestamp WHERE user_id=?", "DELETE FROM account WHERE user_id=?"}) {
        QSqlQuery q(m_db);
        if (!q.prepare(QString::fromLatin1(sql))) {
            m_lastError = q.lastError().text();
            qCritical() << "DeleteAccount: prepare failed:" << m_lastError;
            m_db.rollback();
            return false;
        }
        q.addBindValue(uid);
        if (!Exec(q)) {
            qCritical() << "DeleteAccount: delete failed:" << m_lastError;
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "DeleteAccount: commit failed:" << m_lastError;
        m_db.rollback();
        return false;
    }

    qInfo() << "DeleteAccount: removed user" << userId << "— scores:" << summary.scores
            << "trophies:" << summary.trophies << "tus variables:" << summary.tusVariables
            << "tus data:" << summary.tusData << "relationships:" << summary.friendships;
    return true;
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

void Database::RunMaintenance() {
    // Remove score rows whose communication_id is blank. An empty com id is
    // stored as 12 NUL bytes (GetNpCommId pads a missing id), so a plain '=' '''
    // check would miss them; strip NULs (and coalesce NULL) before comparing.
    const QString blank =
        QStringLiteral("replace(coalesce(communication_id, ''), char(0), '') = ''");
    for (const QString& table : {QStringLiteral("score"), QStringLiteral("score_table")}) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE %2").arg(table, blank));
        if (Exec(q)) {
            const int n = q.numRowsAffected();
            if (n > 0) {
                qInfo() << "Maintenance: removed" << n << "empty-comId row(s) from" << table;
            }
        }
    }
}

// Title name mapping

bool Database::SetTitleName(const QString& comId, const QString& titleName) {
    if (comId.isEmpty() || titleName.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO title_name(communication_id, title_name) "
              "VALUES(?, ?)");
    q.addBindValue(comId);
    q.addBindValue(titleName);
    return Exec(q);
}

QList<Database::KnownTitleRow> Database::ListKnownTitles() {
    QList<KnownTitleRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT c.cid, COALESCE(tn.title_name, ''), "
              "  EXISTS(SELECT 1 FROM score s WHERE s.communication_id = c.cid), "
              "  EXISTS(SELECT 1 FROM user_trophies t WHERE t.communication_id = c.cid), "
              "  EXISTS(SELECT 1 FROM trophy_meta m WHERE m.communication_id = c.cid) "
              "FROM ("
              "  SELECT communication_id AS cid FROM title_name "
              "  UNION SELECT communication_id FROM score "
              "  UNION SELECT communication_id FROM user_trophies "
              "  UNION SELECT communication_id FROM trophy_meta"
              ") c "
              "LEFT JOIN title_name tn ON tn.communication_id = c.cid "
              "WHERE c.cid IS NOT NULL AND c.cid <> '' "
              "ORDER BY c.cid ASC");
    if (!Exec(q))
        return out;
    while (q.next()) {
        KnownTitleRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        r.hasScores = q.value(2).toBool();
        r.hasTrophies = q.value(3).toBool();
        r.hasTrophyNames = q.value(4).toBool();
        out.append(r);
    }
    return out;
}

bool Database::RenameTitle(const QString& comId, const QString& titleName) {
    if (comId.isEmpty() || titleName.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO title_name(communication_id, title_name) VALUES(?, ?) "
              "ON CONFLICT(communication_id) DO UPDATE SET title_name = excluded.title_name");
    q.addBindValue(comId);
    q.addBindValue(titleName);
    return Exec(q);
}

bool Database::ClearTitleName(const QString& comId) {
    if (comId.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM title_name WHERE communication_id=?");
    q.addBindValue(comId);
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

std::optional<QString> Database::GetTitleName(const QString& comId) {
    if (comId.isEmpty())
        return std::nullopt;
    QSqlQuery q(m_db);
    q.prepare("SELECT title_name FROM title_name WHERE communication_id=?");
    q.addBindValue(comId);
    if (!Exec(q) || !q.next())
        return std::nullopt;
    return q.value(0).toString();
}

QList<Database::GameTitleRow> Database::ListScoredGameTitles() {
    QList<GameTitleRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT s.communication_id, COALESCE(tn.title_name, '') AS name "
              "FROM (SELECT DISTINCT communication_id FROM score) s "
              "LEFT JOIN title_name tn ON tn.communication_id = s.communication_id "
              "ORDER BY name ASC, s.communication_id ASC");
    if (!Exec(q))
        return out;
    while (q.next()) {
        GameTitleRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        out.append(r);
    }
    return out;
}

// Leaderboard moderation

QList<Database::BoardRow> Database::ListScoreBoards() {
    QList<BoardRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT s.communication_id, s.board_id, COUNT(*) AS n, "
              "COALESCE(tn.title_name, '') AS name "
              "FROM score s LEFT JOIN title_name tn "
              "  ON tn.communication_id = s.communication_id "
              "GROUP BY s.communication_id, s.board_id "
              "ORDER BY n DESC, name ASC, s.communication_id ASC, s.board_id ASC");
    if (!Exec(q))
        return out;
    while (q.next()) {
        BoardRow r;
        r.comId = q.value(0).toString();
        r.boardId = q.value(1).toUInt();
        r.scoreCount = q.value(2).toInt();
        r.titleName = q.value(3).toString();
        out.append(r);
    }
    return out;
}

QList<Database::BoardScoreRow> Database::ListBoardScores(const QString& comId, uint32_t boardId,
                                                         int limit, int offset) {
    QList<BoardScoreRow> out;
    if (limit <= 0)
        return out;

    QSqlQuery q(m_db);
    q.prepare("SELECT s.user_id, COALESCE(a.username, ''), s.character_id, s.score, "
              "       COALESCE(s.comment, ''), s.data_id, s.timestamp "
              "FROM score s LEFT JOIN account a ON a.user_id = s.user_id "
              "WHERE s.communication_id = ? AND s.board_id = ? "
              "ORDER BY s.score DESC, s.timestamp ASC, s.user_id ASC "
              "LIMIT ? OFFSET ?");
    q.addBindValue(comId);
    q.addBindValue(boardId);
    q.addBindValue(limit);
    q.addBindValue(qMax(0, offset));
    if (!Exec(q))
        return out;

    while (q.next()) {
        BoardScoreRow r;
        r.comId = comId;
        r.boardId = boardId;
        r.userId = q.value(0).toLongLong();
        r.npid = q.value(1).toString();
        r.characterId = q.value(2).toInt();
        r.score = q.value(3).toLongLong();
        r.comment = q.value(4).toString();
        r.dataId = q.value(5).toULongLong();
        r.timestamp = q.value(6).toLongLong();
        out.append(r);
    }
    return out;
}

int Database::CountBoardScores(const QString& comId, uint32_t boardId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM score WHERE communication_id = ? AND board_id = ?");
    q.addBindValue(comId);
    q.addBindValue(boardId);
    return (Exec(q) && q.next()) ? q.value(0).toInt() : 0;
}

bool Database::DeleteScore(const QString& comId, uint32_t boardId, int64_t userId,
                           int32_t characterId, uint64_t& dataId) {
    dataId = 0;

    // Read the blob id before the row goes, or the file becomes unreachable.
    QSqlQuery look(m_db);
    look.prepare("SELECT data_id FROM score WHERE communication_id=? AND board_id=? "
                 "AND user_id=? AND character_id=?");
    look.addBindValue(comId);
    look.addBindValue(boardId);
    look.addBindValue(static_cast<qlonglong>(userId));
    look.addBindValue(characterId);
    if (!Exec(look) || !look.next())
        return false;
    const qlonglong id = look.value(0).toLongLong();
    if (id > 0)
        dataId = static_cast<uint64_t>(id);

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM score WHERE communication_id=? AND board_id=? "
              "AND user_id=? AND character_id=?");
    q.addBindValue(comId);
    q.addBindValue(boardId);
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(characterId);
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

// Trophies

bool Database::RecordUserTrophy(int64_t userId, const QString& comId, int32_t trophyId,
                                int64_t earnedAt) {
    QSqlQuery q(m_db);
    // OR IGNORE, not OR REPLACE: the primary key already holds the first unlock,
    // and a later re-sync should not move the date somebody earned it.
    q.prepare("INSERT OR IGNORE INTO user_trophies(user_id, communication_id, trophy_id, "
              "earned_at) VALUES(?,?,?,?)");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(comId);
    q.addBindValue(trophyId);
    q.addBindValue(static_cast<qlonglong>(earnedAt));
    return Exec(q);
}

bool Database::RecordUserTrophies(int64_t userId, const QString& comId,
                                  const QList<QPair<int32_t, int64_t>>& trophies) {
    if (trophies.isEmpty())
        return true;

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "RecordUserTrophies: cannot start transaction:" << m_lastError;
        return false;
    }

    QSqlQuery q(m_db);
    if (!q.prepare("INSERT OR IGNORE INTO user_trophies(user_id, communication_id, trophy_id, "
                   "earned_at) VALUES(?,?,?,?)")) {
        m_lastError = q.lastError().text();
        m_db.rollback();
        return false;
    }

    // One prepared statement reused per row, rather than a generated
    // multi-row VALUES list: no statement-length ceiling to worry about.
    for (const auto& t : trophies) {
        q.bindValue(0, static_cast<qlonglong>(userId));
        q.bindValue(1, comId);
        q.bindValue(2, t.first);
        q.bindValue(3, static_cast<qlonglong>(t.second));
        if (!Exec(q)) {
            qCritical() << "RecordUserTrophies: insert failed:" << m_lastError;
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

QList<Database::TrophyRow> Database::ListUserTrophies(int64_t userId) {
    QList<TrophyRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT t.communication_id, COALESCE(tn.title_name, ''), t.trophy_id, t.earned_at "
              "FROM user_trophies t "
              "LEFT JOIN title_name tn ON tn.communication_id = t.communication_id "
              "WHERE t.user_id = ? "
              "ORDER BY t.communication_id ASC, t.trophy_id ASC");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        r.trophyId = q.value(2).toInt();
        r.earnedAt = q.value(3).toLongLong();
        out.append(r);
    }
    return out;
}

QList<Database::TrophyRow> Database::ListUserTrophiesForGame(int64_t userId, const QString& comId) {
    QList<TrophyRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT t.communication_id, COALESCE(tn.title_name, ''), t.trophy_id, t.earned_at "
              "FROM user_trophies t "
              "LEFT JOIN title_name tn ON tn.communication_id = t.communication_id "
              "WHERE t.user_id = ? AND t.communication_id = ? "
              "ORDER BY t.trophy_id ASC");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(comId);
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        r.trophyId = q.value(2).toInt();
        r.earnedAt = q.value(3).toLongLong();
        out.append(r);
    }
    return out;
}

QList<Database::TrophyGameRow> Database::ListTrophyGames() {
    QList<TrophyGameRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT t.communication_id, COALESCE(tn.title_name, ''), "
              "       COUNT(DISTINCT t.user_id), COUNT(DISTINCT t.trophy_id), COUNT(*) "
              "FROM user_trophies t "
              "LEFT JOIN title_name tn ON tn.communication_id = t.communication_id "
              "GROUP BY t.communication_id "
              "ORDER BY COUNT(DISTINCT t.user_id) DESC, t.communication_id ASC");
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyGameRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        r.players = q.value(2).toInt();
        r.trophies = q.value(3).toInt();
        r.unlocks = q.value(4).toInt();
        out.append(r);
    }
    return out;
}

QList<Database::TrophyEarnerRow> Database::ListTrophyEarners(const QString& comId) {
    QList<TrophyEarnerRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT trophy_id, COUNT(*) FROM user_trophies WHERE communication_id = ? "
              "GROUP BY trophy_id ORDER BY trophy_id ASC");
    q.addBindValue(comId);
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyEarnerRow r;
        r.trophyId = q.value(0).toInt();
        r.earners = q.value(1).toInt();
        out.append(r);
    }
    return out;
}

Database::TrophySetShape Database::GetTrophySetShape(const QString& comId) {
    TrophySetShape shape;
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*), "
              "  SUM(CASE WHEN grade='B' THEN 1 ELSE 0 END), "
              "  SUM(CASE WHEN grade='S' THEN 1 ELSE 0 END), "
              "  SUM(CASE WHEN grade='G' THEN 1 ELSE 0 END), "
              "  SUM(CASE WHEN grade='P' THEN 1 ELSE 0 END) "
              "FROM trophy_meta WHERE communication_id = ?");
    q.addBindValue(comId);
    if (Exec(q) && q.next()) {
        shape.total = q.value(0).toInt();
        shape.bronze = q.value(1).toInt();
        shape.silver = q.value(2).toInt();
        shape.gold = q.value(3).toInt();
        shape.platinum = q.value(4).toInt();
    }
    return shape;
}

int Database::CountTrophyPlayers(const QString& comId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(DISTINCT user_id) FROM user_trophies WHERE communication_id = ?");
    q.addBindValue(comId);
    return (Exec(q) && q.next()) ? q.value(0).toInt() : 0;
}

QList<Database::TrophyPlayerRow> Database::ListTopTrophyPlayers(const QString& comId, int limit) {
    QList<TrophyPlayerRow> out;
    if (limit <= 0)
        return out;
    QSqlQuery q(m_db);
    // INNER JOIN on purpose: a public leaderboard should not list rows whose
    // account no longer exists.
    q.prepare("SELECT t.user_id, a.username, COUNT(*), MAX(t.earned_at) "
              "FROM user_trophies t JOIN account a ON a.user_id = t.user_id "
              "WHERE t.communication_id = ? "
              "GROUP BY t.user_id "
              "ORDER BY COUNT(*) DESC, MAX(t.earned_at) ASC, a.username ASC "
              "LIMIT ?");
    q.addBindValue(comId);
    q.addBindValue(limit);
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyPlayerRow r;
        r.userId = q.value(0).toLongLong();
        r.npid = q.value(1).toString();
        r.trophies = q.value(2).toInt();
        r.lastEarnedAt = q.value(3).toLongLong();
        out.append(r);
    }
    return out;
}

QList<Database::TrophyProfileRow> Database::ListPlayerTrophySummary(int64_t userId) {
    QList<TrophyProfileRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT t.communication_id, COALESCE(tn.title_name, ''), COUNT(*), "
              "       MIN(t.earned_at), MAX(t.earned_at), "
              "       SUM(CASE WHEN m.grade='B' THEN 1 ELSE 0 END), "
              "       SUM(CASE WHEN m.grade='S' THEN 1 ELSE 0 END), "
              "       SUM(CASE WHEN m.grade='G' THEN 1 ELSE 0 END), "
              "       SUM(CASE WHEN m.grade='P' THEN 1 ELSE 0 END), "
              "       SUM(CASE WHEN m.grade IS NULL OR m.grade='' THEN 1 ELSE 0 END), "
              "       (SELECT COUNT(*) FROM trophy_meta mm "
              "        WHERE mm.communication_id = t.communication_id) "
              "FROM user_trophies t "
              "LEFT JOIN title_name tn ON tn.communication_id = t.communication_id "
              "LEFT JOIN trophy_meta m ON m.communication_id = t.communication_id "
              "                       AND m.trophy_id = t.trophy_id "
              "WHERE t.user_id = ? "
              "GROUP BY t.communication_id "
              "ORDER BY COUNT(*) DESC, t.communication_id ASC");
    q.addBindValue(static_cast<qlonglong>(userId));
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyProfileRow r;
        r.comId = q.value(0).toString();
        r.titleName = q.value(1).toString();
        r.trophies = q.value(2).toInt();
        r.firstEarnedAt = q.value(3).toLongLong();
        r.lastEarnedAt = q.value(4).toLongLong();
        r.bronze = q.value(5).toInt();
        r.silver = q.value(6).toInt();
        r.gold = q.value(7).toInt();
        r.platinum = q.value(8).toInt();
        r.unknownGrade = q.value(9).toInt();
        r.totalInGame = q.value(10).toInt();
        out.append(r);
    }
    return out;
}

Database::TrophyTotals Database::GetTrophyTotals() {
    TrophyTotals t;
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(DISTINCT user_id), COUNT(DISTINCT communication_id), COUNT(*) "
              "FROM user_trophies");
    if (Exec(q) && q.next()) {
        t.players = q.value(0).toInt();
        t.games = q.value(1).toInt();
        t.unlocks = q.value(2).toInt();
    }
    return t;
}

bool Database::ImportTrophyMeta(const QString& comId, const QList<TrophyMetaRow>& rows,
                                const QList<TrophyGroupRow>& groups) {
    if (comId.isEmpty())
        return false;

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        qCritical() << "ImportTrophyMeta: cannot start transaction:" << m_lastError;
        return false;
    }

    // Clear first so trophies dropped from a corrected file do not linger.
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM trophy_meta WHERE communication_id=?");
    del.addBindValue(comId);
    if (!Exec(del)) {
        m_db.rollback();
        return false;
    }

    QSqlQuery delGroups(m_db);
    delGroups.prepare("DELETE FROM trophy_group WHERE communication_id=?");
    delGroups.addBindValue(comId);
    if (!Exec(delGroups)) {
        m_db.rollback();
        return false;
    }

    QSqlQuery g(m_db);
    if (!g.prepare("INSERT INTO trophy_group(communication_id, group_id, name, detail) "
                   "VALUES(?,?,?,?)")) {
        m_lastError = g.lastError().text();
        m_db.rollback();
        return false;
    }
    for (const TrophyGroupRow& r : groups) {
        g.bindValue(0, comId);
        g.bindValue(1, r.groupId);
        g.bindValue(2, r.name);
        g.bindValue(3, r.detail);
        if (!Exec(g)) {
            qCritical() << "ImportTrophyMeta: group insert failed:" << m_lastError;
            m_db.rollback();
            return false;
        }
    }

    QSqlQuery q(m_db);
    if (!q.prepare("INSERT INTO trophy_meta(communication_id, trophy_id, name, detail, grade, "
                   "hidden, group_id, language, imported_at) VALUES(?,?,?,?,?,?,?,?,?)")) {
        m_lastError = q.lastError().text();
        m_db.rollback();
        return false;
    }

    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    for (const TrophyMetaRow& r : rows) {
        q.bindValue(0, comId);
        q.bindValue(1, r.trophyId);
        q.bindValue(2, r.name);
        q.bindValue(3, r.detail);
        q.bindValue(4, r.grade);
        q.bindValue(5, r.hidden ? 1 : 0);
        q.bindValue(6, r.groupId);
        q.bindValue(7, r.language);
        q.bindValue(8, now);
        if (!Exec(q)) {
            qCritical() << "ImportTrophyMeta: insert failed for trophy" << r.trophyId << ":"
                        << m_lastError;
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    qInfo() << "ImportTrophyMeta:" << rows.size() << "trophies for" << comId;
    return true;
}

QList<Database::TrophyGroupRow> Database::ListTrophyGroups(const QString& comId) {
    QList<TrophyGroupRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT group_id, name, COALESCE(detail,'') FROM trophy_group "
              "WHERE communication_id=? ORDER BY group_id ASC");
    q.addBindValue(comId);
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyGroupRow r;
        r.groupId = q.value(0).toInt();
        r.name = q.value(1).toString();
        r.detail = q.value(2).toString();
        out.append(r);
    }
    return out;
}

QList<Database::TrophyMetaRow> Database::ListTrophyMeta(const QString& comId) {
    QList<TrophyMetaRow> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT trophy_id, name, COALESCE(detail,''), COALESCE(grade,''), hidden, "
              "       group_id, COALESCE(language,'') "
              "FROM trophy_meta WHERE communication_id=? ORDER BY trophy_id ASC");
    q.addBindValue(comId);
    if (!Exec(q))
        return out;
    while (q.next()) {
        TrophyMetaRow r;
        r.comId = comId;
        r.trophyId = q.value(0).toInt();
        r.name = q.value(1).toString();
        r.detail = q.value(2).toString();
        r.grade = q.value(3).toString();
        r.hidden = q.value(4).toBool();
        r.groupId = q.value(5).toInt();
        r.language = q.value(6).toString();
        out.append(r);
    }
    return out;
}

int Database::CountTrophyMeta(const QString& comId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM trophy_meta WHERE communication_id=?");
    q.addBindValue(comId);
    return (Exec(q) && q.next()) ? q.value(0).toInt() : 0;
}

bool Database::DeleteTrophyMeta(const QString& comId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM trophy_meta WHERE communication_id=?");
    q.addBindValue(comId);
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
}

bool Database::DeleteUserTrophy(int64_t userId, const QString& comId, int32_t trophyId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM user_trophies WHERE user_id=? AND communication_id=? AND trophy_id=?");
    q.addBindValue(static_cast<qlonglong>(userId));
    q.addBindValue(comId);
    q.addBindValue(trophyId);
    if (!Exec(q))
        return false;
    return q.numRowsAffected() > 0;
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
