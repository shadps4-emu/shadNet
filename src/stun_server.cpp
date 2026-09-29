// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QtEndian>
#include "stream_extractor.h"
#include "stun_server.h"

// shadPS4 sends signaling as a PS4 P2P datagram from and to vport 0xFFFF:
//   [0xFF][0x80 | 3][src vport FFFF][dst vport FFFF][payload]
// Replies use the same header.
static const QByteArray SIGNALING_HEADER("\xFF\x83\xFF\xFF\xFF\xFF", 6);

static QByteArray StripSignalingHeader(const QByteArray& data) {
    if (!data.startsWith(SIGNALING_HEADER))
        return {}; // not a signaling packet
    return data.mid(SIGNALING_HEADER.size());
}

static QByteArray FrameSignaling(const QByteArray& payload) {
    return SIGNALING_HEADER + payload;
}

StunServer::StunServer(SharedState* shared, QObject* parent)
    : QObject(parent), m_socket(new QUdpSocket(this)), m_shared(shared) {
    connect(m_socket, &QUdpSocket::readyRead, this, &StunServer::OnReadyRead);
}

bool StunServer::Start(const QHostAddress& addr, uint16_t port) {
    if (!m_socket->bind(addr, port)) {
        qCritical() << "STUN UDP bind failed on" << addr.toString() << ":" << port
                    << m_socket->errorString();
        return false;
    }
    qInfo().nospace().noquote() << "STUN UDP listener on: " << addr.toString() << ":" << port;
    return true;
}

void StunServer::OnReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(raw.data(), raw.size(), &sender, &senderPort);

        if (raw.isEmpty())
            continue;

        QByteArray data = StripSignalingHeader(raw);
        if (data.isEmpty())
            continue;

        uint8_t cmd = static_cast<uint8_t>(data[0]);
        switch (cmd) {
        case 0x01:
            HandleStunPing(data, sender, senderPort);
            break;
        default:
            break;
        }
    }
}

void StunServer::HandleStunPing(const QByteArray& data, const QHostAddress& sender,
                                uint16_t senderPort) {
    // cmd(1) + online_id(16) + local_ip(4) = 21 bytes minimum
    if (data.size() < 21)
        return;

    // Extract null-padded online_id from bytes 1-16
    QByteArray rawId = data.mid(1, 16);
    int nullPos = rawId.indexOf('\0');
    QString npid = QString::fromUtf8(rawId.left(nullPos >= 0 ? nullPos : 16)).trimmed();
    if (npid.isEmpty())
        return;

    QString extIpStr = sender.toString();
    // Strip IPv6-mapped prefix if present (e.g. "::ffff:192.168.1.1" -> "192.168.1.1")
    if (extIpStr.startsWith("::ffff:"))
        extIpStr = extIpStr.mid(7);

    qInfo() << "STUN ping: npid=" << npid << "ext=" << extIpStr << ":" << senderPort;

    // Record external endpoint
    {
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.udpExt[npid] = {extIpStr, senderPort};
    }

    // Reply: ext_ip(4 bytes network order) + ext_port(2 bytes network order)
    QHostAddress extAddr(extIpStr);
    uint32_t ipNet = qToBigEndian(static_cast<uint32_t>(extAddr.toIPv4Address()));
    uint16_t portNet = qToBigEndian(senderPort);

    QByteArray response;
    response.append(reinterpret_cast<const char*>(&ipNet), 4);
    response.append(reinterpret_cast<const char*>(&portNet), 2);
    m_socket->writeDatagram(FrameSignaling(response), sender, senderPort);
}
