#pragma once
#include <QString>
#include <QStringList>
#include <QSet>
#include <QReadWriteLock>
#include <QReadLocker>
#include <QWriteLocker>

class ConfigManager {
public:
	bool Load(const QString& path = "shadnet.cfg");
	void Reload(const QString& path = "shadnet.cfg");

	QString GetHost()          const { QReadLocker lk(&m_lock); return m_host; }
	QString GetUnsecuredPort()     const { QReadLocker lk(&m_lock); return m_unsecured_port; }
	bool    IsEmailValidated() const { QReadLocker lk(&m_lock); return m_emailValidated; }
	bool    IsBannedDomain(const QString& d) const {
		QReadLocker lk(&m_lock);
		return m_bannedDomains.contains(d.toLower());
	}
	bool IsAdmin(const QString& npid) const {
		QReadLocker lk(&m_lock);
		return m_adminsList.contains(npid);
	}

	void SetHost(const QString& v) { QWriteLocker lk(&m_lock); m_host = v; }
	void SetUnsecuredPort(const QString& v) { QWriteLocker lk(&m_lock); m_unsecured_port = v; }
	void SetEmailValidated(bool v) { QWriteLocker lk(&m_lock); m_emailValidated = v; }
	void SetAdminsList(const QStringList& v) { QWriteLocker lk(&m_lock); m_adminsList = v; }
	void LoadBannedDomains();
private:
	void Parse(const QString& path);

	mutable QReadWriteLock m_lock;
	QString     m_path;

	// config values
	QString     m_host = "0.0.0.0";
	QString     m_unsecured_port = "31313";
	bool        m_emailValidated = false;
	QStringList m_adminsList;
	QSet<QString> m_bannedDomains;
};
