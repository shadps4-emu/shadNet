#include "server.h"
#include <QThread>
#include <QDir>

ShadNetServer::ShadNetServer(QObject* parent)
	: QObject(parent)
	, m_unsecuredServer(new QTcpServer(this))
{
	connect(m_unsecuredServer, &QTcpServer::newConnection, this, &ShadNetServer::OnNewUnsecuredConnection);
}

ShadNetServer::~ShadNetServer()
{
	Stop();
}

bool ShadNetServer::Start(ConfigManager* config)
{
	m_config = config;
	m_shared.config = config;

	m_dbPath = "db/shadnet.db";
	QDir().mkpath("db");

	QHostAddress addr;
	const QString host = config->GetHost();
	if (host.isEmpty() || host == "0.0.0.0") addr = QHostAddress::AnyIPv4;
	else addr = QHostAddress(host);

	uint16_t plainPort = static_cast<uint16_t>(config->GetUnsecuredPort().toUInt());
	if (m_unsecuredServer->listen(addr, plainPort)) {
		qInfo().nospace() << "Unsesured TCP listener on " << addr.toString() << ":" << plainPort
			<< " No encryption use it only on trusted networks";
	}
	else {
		qCritical() << "Plain TCP listen failed:" << m_unsecuredServer->errorString();
	}

	return true;
}

void ShadNetServer::Stop()
{
	if (m_unsecuredServer->isListening()) m_unsecuredServer->close();
}

void ShadNetServer::SpawnSession(QTcpSocket* socket, bool isSsl)
{
	QThread* thread = new QThread;
	ClientSession* session = new ClientSession(socket, &m_shared, m_dbPath, isSsl);
	session->moveToThread(thread);

	connect(thread, &QThread::started, session, &ClientSession::Start);
	connect(session, &ClientSession::Disconnected, thread, &QThread::quit);
	connect(session, &ClientSession::Disconnected, session, &QObject::deleteLater);
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);

	thread->start();
}

void ShadNetServer::OnNewUnsecuredConnection()
{
	while (m_unsecuredServer->hasPendingConnections()) {
		QTcpSocket* sock = m_unsecuredServer->nextPendingConnection();
		// Disconnect from the server's parent so spawnSession can reparent it
		sock->setParent(nullptr);
		SpawnSession(sock, /*isSsl=*/false);
	}
}
