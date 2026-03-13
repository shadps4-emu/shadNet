#include "client_session.h"
#include "protocol.h"
#include "stream_extractor.h"

ClientSession::ClientSession(QTcpSocket* socket, SharedState* shared, const QString& dbPath,
                             bool isSsl, QObject* parent)
    : QObject(parent), m_socket(socket), m_isSsl(isSsl), m_shared(shared),
      m_db(std::make_unique<Database>(QString("sess_%1").arg(reinterpret_cast<quintptr>(this)))) {
    m_socket->setParent(this);
    m_db->Open(dbPath);

    connect(m_socket, &QTcpSocket::readyRead, this, &ClientSession::OnReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ClientSession::OnDisconnected);
}
ClientSession::~ClientSession() {}

void ClientSession::Start() {
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

void ClientSession::OnReadyRead() {
    m_readBuf.append(m_socket->readAll());

    while (m_readBuf.size() >= static_cast<int>(HEADER_SIZE)) {
        // Parse header
        const uint8_t* hdr = reinterpret_cast<const uint8_t*>(m_readBuf.constData());

        if (hdr[0] != static_cast<uint8_t>(PacketType::Request)) {
            // Only Request packets (type 0) come from the client.
            // Anything else is a protocol violation — drop the connection.
            qWarning() << "Unexpected packet type" << hdr[0] << "— disconnecting";
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

void ClientSession::OnDisconnected() {
    // TODO
    CleanupOnDisconnect();
    emit Disconnected();
}

void ClientSession::ProcessPacket(uint16_t command, uint64_t packetId, const QByteArray& payload) {
    // TODO partially done

    QByteArray reply;
    reply.reserve(256);
    reply.append(static_cast<char>(static_cast<uint8_t>(PacketType::Reply)));
    appendU16LE(reply, command);
    appendU32LE(reply, HEADER_SIZE); // placeholder, fixed up below
    appendU64LE(reply, packetId);
    reply.append(
        static_cast<char>(static_cast<uint8_t>(ErrorType::NoError))); // error byte placeholder

    StreamExtractor se(payload);

    auto cmdOpt = static_cast<CommandType>(command);

    ErrorType result = DispatchCommand(cmdOpt, se, reply);
    reply[static_cast<int>(HEADER_SIZE)] = static_cast<char>(static_cast<uint8_t>(result));
    fixPacketSize(reply);
    SendPacket(reply);

    if (result == ErrorType::Malformed) {
        qWarning() << "Malformed command" << command << "- disconnecting";
        m_socket->disconnectFromHost();
    }
}

ErrorType ClientSession::DispatchCommand(CommandType cmd, StreamExtractor& se, QByteArray& reply) {
    qDebug() << "Command:" << static_cast<uint16_t>(cmd);

    // Terminate is always allowed regardless of auth state.
    // Send the reply first, then close — client expects the reply byte.
    if (cmd == CommandType::Terminate) {
        QMetaObject::invokeMethod(m_socket, "disconnectFromHost", Qt::QueuedConnection);
        return ErrorType::NoError;
    }

    // Pre-auth: only Login and Create are permitted.
    if (!m_authenticated) {
        switch (cmd) {
        case CommandType::Login:
            return CmdLogin(se, reply);
        case CommandType::Create:
            return CmdCreate(se, reply);
        default:
            qWarning() << "Command" << static_cast<uint16_t>(cmd) << "requires authentication";
            return ErrorType::Unauthorized;
        }
    }

    // Authenticated commands.
    switch (cmd) {
    case CommandType::Login:
        return CmdLogin(se, reply);
    case CommandType::Create:
        return CmdCreate(se, reply);
    default:
        qWarning() << "Unknown command" << static_cast<uint16_t>(cmd);
        return ErrorType::Invalid;
    }
}

void ClientSession::CleanupOnDisconnect() {
    if (m_authenticated) {
        QWriteLocker lk(&m_shared->clientsLock);
        m_shared->clients.remove(m_info.userId);
        qInfo() << "Client disconnected:" << m_info.npid;
    } else {
        qInfo() << "Unauthenticated client disconnected";
    }
}

void ClientSession::SendPacket(const QByteArray& pkt) {
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(pkt);
        m_socket->flush();
    }
}
