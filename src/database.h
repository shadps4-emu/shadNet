#pragma once
#include "protocol.h"
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QMutex>
#include <QDateTime>
#include <optional>

enum class DbError {
    None = 0,              // No error (success)
    ExistingUsername,      // Username already exists
    ExistingEmail,         // Email already exists
    InvalidInput,          // Invalid input parameters
    InvalidEmail,          // Invalid email format
    Internal,              // Internal database error
    ConnectionError,       // Database connection error
    TransactionError       // Transaction failed
};

class Database {
public:
    explicit Database(const QString& connectionName = "default");
    ~Database();

    bool Open(const QString& path = "db/rpcn.db");
    bool Migrate();
    bool IsOpen() const;

    // Account
    std::optional<DbError> CreateAccount(const QString& npid, const QString& password,
        const QString& onlineName, const QString& avatarUrl,
        const QString& email);

    QString lastError() const { return m_lastError; }

private:
    bool Exec(const QString& sql);
    bool Exec(QSqlQuery& q);
    QByteArray HashPassword(const QString& password, const QByteArray& salt);
    QByteArray GenerateSalt(int bytes = 64);
    QString    GenerateToken(int len = 16);

    QSqlDatabase m_db;
    QString      m_connName;
    mutable QString m_lastError;
};