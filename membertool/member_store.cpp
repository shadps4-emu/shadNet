// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "avatar.h"
#include "member_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRandomGenerator>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

namespace {

QString GeneratePassword() {
    const QString alphabet =
        QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789");
    QString password;
    for (int i = 0; i < 16; ++i)
        password.append(
            alphabet.at(QRandomGenerator::system()->bounded(static_cast<int>(alphabet.size()))));
    return password;
}

bool ValidUsername(const QString& username) {
    if (username.size() < 3 || username.size() > 16 || username == "DeletedUser")
        return false;
    for (QChar c : username)
        if (!c.isLetterOrNumber() && c != '-' && c != '_')
            return false;
    return true;
}

bool HasMemberTables(const QSqlDatabase& database, QString& error) {
    const QMap<QString, QStringList> required = {
        {"account",
         {"user_id", "username", "hash", "salt", "avatar_url", "email", "email_check", "token",
          "reset_token", "admin", "stat_agent", "banned"}},
        {"account_timestamp", {"user_id", "creation"}},
        {"friendship", {"user_id_1", "user_id_2"}},
        {"score", {"user_id", "data_id"}},
        {"user_trophies", {"user_id"}},
        {"tus_variable", {"owner_user_id"}},
        {"tus_data", {"owner_user_id"}},
    };
    for (auto it = required.cbegin(); it != required.cend(); ++it) {
        const QSqlRecord record = database.record(it.key());
        for (const QString& column : it.value()) {
            if (!record.contains(column)) {
                error =
                    QStringLiteral("This is not a current shadnet database. Start the server "
                                   "once to initialize its database, then stop it and try again.");
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool MemberStore::Open(const QString& path, QString& error) {
    error.clear();
    const QFileInfo file(path);
    if (!file.isFile()) {
        error = QStringLiteral("Choose an existing shadnet database.");
        return false;
    }

    // Check the selected file before the server's normal Open runs migrations.
    const QString connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool valid = false;
    {
        auto probe = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        probe.setDatabaseName(file.canonicalFilePath());
        probe.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (probe.open())
            valid = HasMemberTables(probe, error);
        else
            error = probe.lastError().text();
        probe.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (!valid)
        return false;

    auto database = std::make_unique<Database>(QString());
    if (!database->Open(file.canonicalFilePath())) {
        error = database->lastError();
        return false;
    }
    m_db = std::move(database);
    m_path = file.canonicalFilePath();
    return true;
}

bool MemberStore::NeedsToken() const {
    const QDir runtime(QFileInfo(m_path).absoluteDir().absoluteFilePath(".."));
    QSettings config(runtime.filePath("shadnet.cfg"), QSettings::IniFormat);
    return config.value("EmailValidated", false).toBool();
}

QList<Member> MemberStore::List(QString& error) const {
    error.clear();
    QList<Member> members;
    if (!m_db) {
        error = QStringLiteral("Open a database first.");
        return members;
    }
    QSqlQuery query(m_db->Conn());
    if (!query.exec(
            "SELECT user_id, username, email FROM account ORDER BY username COLLATE NOCASE")) {
        error = query.lastError().text();
        return members;
    }
    while (query.next())
        members.append(
            {query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString()});
    return members;
}

std::optional<MemberCredentials> MemberStore::Add(const QString& username, QString& error) {
    error.clear();
    if (!m_db) {
        error = QStringLiteral("Open a database first.");
        return std::nullopt;
    }
    const QString name = username.trimmed();
    if (!ValidUsername(name)) {
        error = QStringLiteral("Use 3–16 letters, numbers, underscores, or hyphens. "
                               "DeletedUser is reserved.");
        return std::nullopt;
    }
    MemberCredentials credentials{name, name + "@shadps4.local", GeneratePassword(), {}};
    const auto result =
        m_db->CreateAccount(name, credentials.password, Avatar::DefaultName(), credentials.email);
    if (result) {
        if (*result == DbError::ExistingUsername)
            error = QStringLiteral("That username already exists.");
        else if (*result == DbError::ExistingEmail)
            error = QStringLiteral("That email address is already in use.");
        else
            error = QStringLiteral("The account could not be created. %1").arg(m_db->lastError());
        return std::nullopt;
    }
    // Exercise the same credential check used by game login and supply its token
    // for servers that require EmailValidated.
    const auto user = m_db->CheckUser(name, credentials.password, QString(), false);
    if (user)
        credentials.token = user->token;
    return credentials;
}

bool MemberStore::Remove(int64_t id, QString& error, QString& warning) {
    error.clear();
    warning.clear();
    if (!m_db) {
        error = QStringLiteral("Open a database first.");
        return false;
    }
    if (!m_db->GetUsername(id)) {
        error = QStringLiteral("That account no longer exists. Refresh the list.");
        return false;
    }
    PurgeSummary removed;
    if (!m_db->DeleteAccount(id, removed)) {
        error = m_db->lastError();
        return false;
    }
    // The server's database is in db/, with score_data/ beside that directory.
    // For a copied database elsewhere, leave file cleanup to the server on restart.
    const QDir databaseDir = QFileInfo(m_path).absoluteDir();
    if (databaseDir.dirName().compare("db", Qt::CaseInsensitive) == 0) {
        const QDir scores(databaseDir.absoluteFilePath("../score_data"));
        for (uint64_t id : removed.scoreDataIds) {
            const QString file = scores.filePath(QString("%1.sdt").arg(id, 20, 10, QChar('0')));
            if (QFile::exists(file) && !QFile::remove(file))
                warning = QStringLiteral(
                    "The account was removed, but some score files could not be deleted.");
        }
    }
    return true;
}
