#include "database.h"
#include <QSqlError>
#include <QSqlRecord>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QUuid>
#include <QDateTime>
#include <QRandomGenerator>
#include <qcryptographichash.h>

static QByteArray GenerateSalt(int bytes = 64)
{
	QByteArray salt(bytes, Qt::Uninitialized);
	QRandomGenerator* generator = QRandomGenerator::global();

	for (int i = 0; i < bytes; ++i) {
		salt[i] = static_cast<char>(generator->bounded(256));
	}

	return salt;
}
static QString GenerateToken(int len = 16)
{
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

QByteArray Database::HashPassword(const QString& password, const QByteArray& salt)
{
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
	: m_connName(connectionName.isEmpty()
		? QUuid::createUuid().toString(QUuid::WithoutBraces)
		: connectionName)
{
}

Database::~Database()
{
	if (m_db.isOpen()) m_db.close();
	m_db = QSqlDatabase{};                    // release the reference first
	QSqlDatabase::removeDatabase(m_connName); // now safe to remove
}

bool Database::IsOpen() const { return m_db.isOpen(); }

bool Database::Open(const QString& path)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	m_db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
	m_db.setDatabaseName(path);
	if (!m_db.open()) { m_lastError = m_db.lastError().text(); return false; }
	Exec("PRAGMA journal_mode=WAL");
	Exec("PRAGMA foreign_keys=ON");
	return Migrate();
}

bool Database::Exec(const QString& sql)
{
	QSqlQuery q(m_db);
	if (!q.exec(sql)) { m_lastError = q.lastError().text(); return false; }
	return true;
}
bool Database::Exec(QSqlQuery& q)
{
	if (!q.exec()) { m_lastError = q.lastError().text(); return false; }
	return true;
}



bool Database::Migrate()
{
	return false;//TODO table creation and migration logic goes here
}



std::optional<DbError> Database::CreateAccount(const QString& npid, const QString& password, const QString& onlineName, const QString& avatarUrl, const QString& email)
{
	return std::optional<DbError>();
}
