#include "client_session.h"
#include "protocol.h"
#include "stream_extractor.h"

ClientSession::ClientSession(QTcpSocket* socket, SharedState* shared, const QString& dbPath, bool isSsl, QObject* parent)
	: QObject(parent), m_socket(socket), m_isSsl(isSsl), m_shared(shared)
{
	m_socket->setParent(this);

	connect(m_socket, &QTcpSocket::readyRead, this, &ClientSession::OnReadyRead);
	connect(m_socket, &QTcpSocket::disconnected, this, &ClientSession::OnDisconnected);

}
ClientSession::~ClientSession()
{
}

void ClientSession::Start()
{
	// Send ServerInfo packet
	QByteArray si;
	si.append(static_cast<char>(static_cast<uint8_t>(PacketType::ServerInfo)));
	appendU16LE(si, 0);
	appendU32LE(si, static_cast<uint32_t>(HEADER_SIZE + 4));
	appendU64LE(si, 0);
	appendU32LE(si, PROTOCOL_VERSION);
	m_socket->write(si);
	m_socket->flush();

	const char* mode = m_isSsl ? "TLS" : "plain";
	qInfo() << "Client connected (" << mode << ") from" << m_socket->peerAddress().toString();
}

void ClientSession::OnReadyRead()
{
	m_readBuf.append(m_socket->readAll());

	while (m_readBuf.size() >= static_cast<int>(HEADER_SIZE)) {
		// Parse header
		const uint8_t* hdr = reinterpret_cast<const uint8_t*>(m_readBuf.constData());

		if (hdr[0] != static_cast<uint8_t>(PacketType::Request)) {
			qWarning() << "Non-request packet, disconnecting";
			m_socket->disconnectFromHost();
			return;
		}

		uint16_t command;
		std::memcpy(&command, hdr + 1, 2);
		// assume little-endian host
		uint32_t pktSize;
		std::memcpy(&pktSize, hdr + 3, 4);
		uint64_t pktId;
		std::memcpy(&pktId, hdr + 7, 8);

		if (pktSize > MAX_PACKET_SIZE) {
			qWarning() << "Oversized packet" << pktSize;
			m_socket->disconnectFromHost();
			return;
		}
		if (m_readBuf.size() < static_cast<int>(pktSize))
			break; // not enough data yet

		QByteArray payload = m_readBuf.mid(HEADER_SIZE, pktSize - HEADER_SIZE);
		m_readBuf.remove(0, pktSize);

		ProcessPacket(command, pktId, payload);
	}
}

void ClientSession::OnDisconnected()
{
	//TODO
	CleanupOnDisconnect();
	emit Disconnected();
}

void ClientSession::ProcessPacket(uint16_t command, uint64_t packetId, const QByteArray& payload)
{
	//TODO
}

void ClientSession::CleanupOnDisconnect()
{
	//TODO
}