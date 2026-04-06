// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QtEndian>
#include "stream_extractor.h"
#include "stun_server.h"

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
    qInfo().nospace() << "STUN UDP listener on " << addr.toString() << ":" << port;
    return true;
}

void StunServer::OnReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(data.data(), data.size(), &sender, &senderPort);

        if (data.isEmpty())
            continue;

        uint8_t cmd = static_cast<uint8_t>(data[0]);
        switch (cmd) {
        case 0x01:
            HandleStunPing(data, sender, senderPort);
            break;
        case 0x02:
            HandleSignalingEstablished(data, sender, senderPort);
            break;
        case 0x04:
            HandleActivationIntent(data, sender, senderPort);
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
    m_socket->writeDatagram(response, sender, senderPort);
}

void StunServer::HandleSignalingEstablished(const QByteArray& data, const QHostAddress& sender,
                                            uint16_t senderPort) {
    // cmd(1) + me_id(16) + peer_id(16) = 33 bytes
    if (data.size() < 33)
        return;

    QByteArray rawMe = data.mid(1, 16);
    QByteArray rawPeer = data.mid(17, 16);
    int nullMe = rawMe.indexOf('\0');
    int nullPeer = rawPeer.indexOf('\0');
    QString meId = QString::fromUtf8(rawMe.left(nullMe >= 0 ? nullMe : 16)).trimmed();
    QString peerId = QString::fromUtf8(rawPeer.left(nullPeer >= 0 ? nullPeer : 16)).trimmed();
    if (meId.isEmpty() || peerId.isEmpty())
        return;

    QString senderIp = sender.toString();
    if (senderIp.startsWith("::ffff:"))
        senderIp = senderIp.mid(7);

    qInfo() << "SignalingEstablished UDP: me=" << meId << "peer=" << peerId << "addr=" << senderIp
            << ":" << senderPort;

    QPair<QString, QString> forwardKey(meId, peerId);
    QPair<QString, QString> reverseKey(peerId, meId);
    QPair<QString, uint16_t> reverseAddr;
    bool mutualActivated = false;

    {
        QWriteLocker lk(&m_shared->matching.signalingLock);
        m_shared->matching.signalingPairs[forwardKey] = {senderIp, senderPort};

        auto revIt = m_shared->matching.signalingPairs.find(reverseKey);
        if (revIt != m_shared->matching.signalingPairs.end()) {
            reverseAddr = *revIt;
            m_shared->matching.signalingPairs.remove(forwardKey);
            m_shared->matching.signalingPairs.remove(reverseKey);
            mutualActivated = true;
        }
    }

    if (mutualActivated) {
        qInfo() << "MutualActivated:" << meId << "<->" << peerId;

        // Send cmd=0x03 + peer's npid (16 bytes) to each side
        QByteArray meIdBytes = meId.toUtf8().left(16);
        meIdBytes.append(QByteArray(16 - meIdBytes.size(), '\0'));
        QByteArray peerIdBytes = peerId.toUtf8().left(16);
        peerIdBytes.append(QByteArray(16 - peerIdBytes.size(), '\0'));

        // To me_id's address: send peer_id
        QByteArray toMe;
        toMe.append(static_cast<char>(0x03));
        toMe.append(peerIdBytes);
        m_socket->writeDatagram(toMe, sender, senderPort);

        // To peer_id's address: send me_id
        QByteArray toPeer;
        toPeer.append(static_cast<char>(0x03));
        toPeer.append(meIdBytes);
        m_socket->writeDatagram(toPeer, QHostAddress(reverseAddr.first), reverseAddr.second);
    }
}

void StunServer::HandleActivationIntent(const QByteArray& data, const QHostAddress& sender,
                                        uint16_t senderPort) {
    Q_UNUSED(senderPort);
    // cmd(1) + me_id(16) + peer_id(16) + ctx_tag(4) = 37 bytes
    if (data.size() < 37)
        return;

    QByteArray rawMe = data.mid(1, 16);
    QByteArray rawPeer = data.mid(17, 16);
    int nullMe = rawMe.indexOf('\0');
    int nullPeer = rawPeer.indexOf('\0');
    QString meId = QString::fromUtf8(rawMe.left(nullMe >= 0 ? nullMe : 16)).trimmed();
    QString peerId = QString::fromUtf8(rawPeer.left(nullPeer >= 0 ? nullPeer : 16)).trimmed();
    if (meId.isEmpty() || peerId.isEmpty())
        return;

    // ctx_tag is u32 little-endian at offset 33
    uint32_t ctxTag;
    memcpy(&ctxTag, data.constData() + 33, 4);
    // ctxTag is already LE on LE host

    // Convert sender IP to network-order u32
    QString senderIp = sender.toString();
    if (senderIp.startsWith("::ffff:"))
        senderIp = senderIp.mid(7);
    QHostAddress senderAddr(senderIp);
    uint32_t initiatorIpInt = qToBigEndian(static_cast<uint32_t>(senderAddr.toIPv4Address()));

    QPair<uint32_t, uint32_t> key(initiatorIpInt, ctxTag);

    qInfo() << "ActivationIntent: me=" << meId << "peer=" << peerId
            << "ctx_tag=0x" + QString::number(ctxTag, 16) << "addr=" << senderIp;

    {
        QWriteLocker lk(&m_shared->matching.activationLock);
        m_shared->matching.activationIntents[key] = {meId, peerId};
    }
}
