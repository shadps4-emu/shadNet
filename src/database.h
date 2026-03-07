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

struct UserRecord {
	int64_t   userId = 0;
	QString   username;
	QByteArray hash;
	QByteArray salt;
	QString   onlineName;
	QString   avatarUrl;
	QString   email;
	QString   emailCheck;
	QString   token;
	bool      admin = false;
	bool      statAgent = false;
	bool      banned = false;
};

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
	std::optional<UserRecord> CheckUser(const QString& npid, const QString& password,
		const QString& token, bool checkToken);
	std::optional<int64_t>    GetUserId(const QString& npid);
	std::optional<QString>    GetUsername(int64_t userId);
	QList<QPair<int64_t, QString>> GetUsernamesFromIds(const QSet<int64_t>& ids);
	bool UpdateLoginTime(int64_t userId);
	bool BanUser(int64_t userId, bool ban);
	bool DeleteUser(int64_t userId);
	bool SetAdmin(int64_t userId, bool admin);
	int  TotalUsers();
	void CleanNeverUsedAccounts();

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