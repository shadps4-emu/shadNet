#pragma once
#include "config.h"
#include <QObject>
#include <QTcpServer>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QList>
#include <memory>

class ShadNetServer : public QObject {
	Q_OBJECT
public:
	explicit ShadNetServer(QObject* parent = nullptr);
	~ShadNetServer();

	bool Start(ConfigManager* config);
	void Stop();

private slots:
	void OnNewUnsecuredConnection();

private:
	void SpawnSession(QTcpSocket* socket, bool isSsl);

	ConfigManager* m_config = nullptr;
	QTcpServer* m_unsecuredServer = nullptr;   // plain TCP connections

	QString           m_dbPath;
};
