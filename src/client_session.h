#pragma once
#include "config.h"
#include "protocol.h"
#include "stream_extractor.h"
#include <QObject>
#include <QTcpSocket>
#include <QSslSocket>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QReadWriteLock>
#include <QAtomicInt>
#include <memory>
#include <functional>

// Shared state visible to all sessions (thread-safe with locks)
struct SharedState {
	ConfigManager* config;
	// Connected clients: userId to (npid, channel write function)
	mutable QReadWriteLock clientsLock;
};

class ClientSession : public QObject {
	Q_OBJECT
public:
	explicit ClientSession(QTcpSocket* socket, SharedState* shared,
		const QString& dbPath, bool isSsl = true,
		QObject* parent = nullptr);
	~ClientSession();

	void Start();
	void CleanupOnDisconnect();
	void SendPacket(const QByteArray& pkt);
signals:
	void Disconnected();

private slots:
	void OnReadyRead();
	void OnDisconnected();

private:
	void ProcessPacket(uint16_t command, uint64_t packetId, const QByteArray& payload);
	ErrorType DispatchCommand(CommandType cmd, StreamExtractor& se, QByteArray& reply);

	QTcpSocket* m_socket;
	bool           m_isSsl = true;
	SharedState* m_shared;
	bool           m_authenticated = false;
	QByteArray     m_readBuf;

};