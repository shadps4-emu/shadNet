// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include "client_session.h"
#include "proto_utils.h"
#include "protocol.h"
#include "shadnet.pb.h"
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
    CleanupOnDisconnect();
    emit Disconnected();
}

void ClientSession::ProcessPacket(uint16_t command, uint64_t packetId, const QByteArray& payload) {
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

// True for authenticated commands whose payload begins with a 12-byte ComId
// score. Used to attribute live-usage game activity in the dispatcher.
static bool LeadsWithComId(CommandType cmd) {
    switch (cmd) {
    case CommandType::GetBoardInfos:
    case CommandType::RecordScore:
    case CommandType::RecordScoreData:
    case CommandType::GetScoreData:
    case CommandType::GetScoreRange:
    case CommandType::GetScoreFriends:
    case CommandType::GetScoreNpid:
    case CommandType::GetScoreAccountId:
    case CommandType::GetScoreGameDataByAccId:
        return true;
    default:
        return false;
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
    // Attribute live-usage game activity: score payloads lead with a 12-byte
    // ComId; peek it (non-consuming) so the handler's own parse is undisturbed.
    if (LeadsWithComId(cmd)) {
        const QByteArray cid = se.peekBytes(12);
        if (cid.size() == 12) {
            // comId newly known/changed -> same-comId friends learn our title info.
            if (m_shared->UsageTouchGame(m_info.userId,
                                         QString::fromLatin1(cid.constData(), cid.size())))
                EmitPresenceGameTitleInfo();
        }
    }
    // Authenticated commands.
    switch (cmd) {
    case CommandType::Login:
        return CmdLogin(se, reply);
    case CommandType::Create:
        return CmdCreate(se, reply);
    case CommandType::Delete:
        return CmdDelete(se);
    case CommandType::AddFriend:
        return CmdAddFriend(se);
    case CommandType::RemoveFriend:
        return CmdRemoveFriend(se);
    case CommandType::AddBlock:
        return CmdAddBlock(se);
    case CommandType::RemoveBlock:
        return CmdRemoveBlock(se);
    case CommandType::SetAppearOffline:
        return CmdSetAppearOffline(se);
    case CommandType::GetBoardInfos:
        return CmdGetBoardInfos(se, reply);
    case CommandType::RecordScore:
        return CmdRecordScore(se, reply);
    case CommandType::RecordScoreData:
        return CmdRecordScoreData(se);
    case CommandType::GetScoreData:
        return CmdGetScoreData(se, reply);
    case CommandType::GetScoreRange:
        return CmdGetScoreRange(se, reply);
    case CommandType::GetScoreFriends:
        return CmdGetScoreFriends(se, reply);
    case CommandType::GetScoreNpid:
        return CmdGetScoreNpid(se, reply);
    // Matchmaking
    case CommandType::ContextStart:
        return CmdContextStart(se);
    case CommandType::ContextStop:
        return CmdContextStop(se);
    case CommandType::CreateRoom:
        return CmdCreateRoom(se, reply);
    case CommandType::JoinRoom:
        return CmdJoinRoom(se, reply);
    case CommandType::LeaveRoom:
        return CmdLeaveRoom(se, reply);
    case CommandType::SearchRoom:
        return CmdSearchRoom(se, reply);
    case CommandType::RequestSignalingInfos:
        return CmdRequestSignalingInfos(se, reply);
    case CommandType::SetRoomDataInternal:
        return CmdSetRoomDataInternal(se, reply);
    case CommandType::SetRoomDataExternal:
        return CmdSetRoomDataExternal(se, reply);
    case CommandType::KickoutRoomMember:
        return CmdKickoutRoomMember(se, reply);
    case CommandType::GetWorldInfoList:
        return CmdGetWorldInfoList(se, reply);
    case CommandType::GetScoreAccountId:
        return CmdGetScoreAccountId(se, reply);
    case CommandType::GetScoreGameDataByAccId:
        return CmdGetScoreGameDataByAccId(se, reply);
    case CommandType::GetToken:
        return CmdGetToken(reply);
    default:
        qWarning() << "Unknown command" << static_cast<uint16_t>(cmd);
        return ErrorType::Invalid;
    }
}

void ClientSession::CleanupOnDisconnect() {
    if (!m_authenticated) {
        qInfo() << "Unauthenticated client disconnected";
        return;
    }

    // Leave any matchmaking room before tearing down the client entry
    CleanupMatchingOnDisconnect();

    // Collect send functions for every online friend before releasing the lock,
    // then remove ourselves from the map.
    QVector<QPair<std::function<void(QByteArray)>, QString>> friendSenders; // (send, npid)
    bool selfAppearOffline = false;
    {
        QWriteLocker lk(&m_shared->clientsLock);
        auto self = m_shared->clients.find(m_info.userId);
        if (self != m_shared->clients.end()) {
            selfAppearOffline = self->appearOffline;
            for (auto it = self->friends.begin(); it != self->friends.end(); ++it) {
                auto friendEntry = m_shared->clients.find(it.key());
                if (friendEntry != m_shared->clients.end())
                    friendSenders.append({friendEntry->send, friendEntry->npid});
            }
            m_shared->clients.erase(self);
            m_shared->npidToUserId.remove(m_info.npid);
        }
    }

    // Drop this session from live usage stats (decrements total + its game tally).
    m_shared->UsageOnLogout(m_info.userId);

    // Build FriendStatus offline notification using the NotifyFriendStatus
    shadnet::NotifyFriendStatus ns;
    ns.set_npid(m_info.npid.toStdString());
    ns.set_online(false);
    ns.set_timestamp(static_cast<uint64_t>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) *
                     1'000'000ULL);

    QByteArray payload;
    const std::string s = ns.SerializeAsString();
    appendBlob(payload, QByteArray(s.data(), static_cast<int>(s.size())));
    QByteArray pkt = BuildNotification(NotificationType::FriendStatus, payload);
    // WebApi onlineStatus presence update (went offline)
    if (!selfAppearOffline) {
        for (const auto& [send, friendNpid] : friendSenders) {
            send(pkt);
            send(BuildNotification(
                NotificationType::WebApiPushEvent,
                BuildWebApiPushPayload(QString(), 0,
                                       QStringLiteral("np:service:presence:onlineStatus"),
                                       QByteArray(), m_info.npid, friendNpid)));
        }
    }

    qInfo() << "Client disconnected:" << m_info.npid;
}

// ── Notification helpers ──────────────────────────────────────────────────────

QByteArray ClientSession::BuildNotification(NotificationType type, const QByteArray& payload) {
    QByteArray pkt;
    pkt.append(static_cast<char>(static_cast<uint8_t>(PacketType::Notification)));
    appendU16LE(pkt, static_cast<uint16_t>(type));
    appendU32LE(pkt, static_cast<uint32_t>(HEADER_SIZE + payload.size()));
    appendU64LE(pkt, 0); // packet_id unused for notifications
    pkt.append(payload);
    return pkt;
}

// Send a notification to another online user.
void ClientSession::SendNotification(NotificationType type, const QByteArray& payload,
                                     int64_t targetUserId) {
    std::function<void(QByteArray)> sender;
    {
        QReadLocker lk(&m_shared->clientsLock);
        auto it = m_shared->clients.find(targetUserId);
        if (it == m_shared->clients.end())
            return;
        sender = it->send;
    }
    sender(BuildNotification(type, payload));
}

// Send a notification to this session's own socket.
void ClientSession::SendSelfNotification(NotificationType type, const QByteArray& payload) {
    SendPacket(BuildNotification(type, payload));
}

QByteArray ClientSession::BuildWebApiPushPayload(const QString& npServiceName,
                                                 quint32 npServiceLabel, const QString& dataType,
                                                 const QByteArray& data, const QString& fromNpid,
                                                 const QString& toNpid,
                                                 const QList<QPair<QString, QString>>& extdData) {
    QByteArray payload;
    appendBlob(payload, npServiceName.toUtf8());
    appendU32LE(payload, npServiceLabel);
    appendBlob(payload, dataType.toUtf8());
    appendBlob(payload, data);
    appendBlob(payload, fromNpid.toUtf8());
    appendBlob(payload, toNpid.toUtf8());
    appendU32LE(payload, static_cast<quint32>(extdData.size()));
    for (const auto& kv : extdData) {
        appendBlob(payload, kv.first.toUtf8());
        appendBlob(payload, kv.second.toUtf8());
    }
    return payload;
}

// Mid-session Appear-Offline toggle. Applies the flag, then notifies friends of the
// resulting online-status change
ErrorType ClientSession::CmdSetAppearOffline(StreamExtractor& data) {
    shadnet::SetAppearOfflineRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;
    const bool enable = req.appear_offline();

    QString myNpid, gameStatusVal, gameDataVal;
    bool hasGameStatus = false, hasGameData = false, hasTitle = false, notifyWithData = false;
    QList<QPair<int64_t, QString>> friends; // (userId, npid) of online friends
    {
        QWriteLocker lk(&m_shared->clientsLock);
        auto self = m_shared->clients.find(m_info.userId);
        if (self == m_shared->clients.end() || self->appearOffline == enable)
            return ErrorType::NoError; // unregistered or no change
        self->appearOffline = enable;
        myNpid = self->npid;
        hasGameStatus = !self->gameStatus.isEmpty();
        hasGameData = !self->gameData.isEmpty();
        hasTitle = !self->npTitleId.isEmpty() || !self->titleName.isEmpty();
        notifyWithData = self->notifyWithData;
        gameStatusVal = self->gameStatus;
        gameDataVal = self->gameData;
        for (auto fr = self->friends.cbegin(); fr != self->friends.cend(); ++fr) {
            auto fit = m_shared->clients.find(fr.key());
            if (fit != m_shared->clients.end() && fit->send)
                friends.append({fr.key(), fit->npid});
        }
    }
    qInfo() << "Appear-Offline" << (enable ? "enabled" : "disabled") << "for" << myNpid;

    const bool nowOnline = !enable;
    static const QString kOnlineStatus = QStringLiteral("np:service:presence:onlineStatus");
    // 1) onlineStatus to ALL online friends (service None, not comId-gated).
    for (const auto& f : friends)
        PushWebApiEvent(QString(), 0, kOnlineStatus, QByteArray(), myNpid, f.second, f.first);

    // 2) in-game events to SAME-comId friends, only while in a game.
    QString myComId;
    QSet<int64_t> sameComId;
    {
        QReadLocker ul(&m_shared->usageLock);
        myComId = m_shared->usageClientGame.value(m_info.userId);
        if (!myComId.isEmpty()) {
            for (auto it = m_shared->usageClientGame.cbegin();
                 it != m_shared->usageClientGame.cend(); ++it) {
                if (it.value() == myComId)
                    sameComId.insert(it.key());
            }
        }
    }
    if (!myComId.isEmpty()) {
        static const QString kInGame = QStringLiteral("inGamePresence");
        static const QString kTitle = QStringLiteral("np:service:presence:gameTitleInfo");
        static const QString kStatus = QStringLiteral("np:service:presence:gameStatus");
        static const QString kData = QStringLiteral("np:service:presence:gameData");
        for (const auto& f : friends) {
            if (!sameComId.contains(f.first))
                continue;
            if (hasTitle)
                PushWebApiEvent(kInGame, 0, kTitle, QByteArray(), myNpid, f.second, f.first);
            if (hasGameStatus) {
                QByteArray body;
                if (nowOnline && notifyWithData) {
                    QJsonObject o;
                    o.insert(QStringLiteral("gameStatus"), gameStatusVal);
                    body = QJsonDocument(o).toJson(QJsonDocument::Compact);
                }
                PushWebApiEvent(kInGame, 0, kStatus, body, myNpid, f.second, f.first);
            }
            if (hasGameData) {
                QByteArray body;
                if (nowOnline && notifyWithData) {
                    QJsonObject o;
                    o.insert(QStringLiteral("gameData"), gameDataVal);
                    body = QJsonDocument(o).toJson(QJsonDocument::Compact);
                }
                PushWebApiEvent(kInGame, 0, kData, body, myNpid, f.second, f.first);
            }
        }
    }
    return ErrorType::NoError;
}

void ClientSession::EmitPresenceGameTitleInfo() {
    // np:service:presence:gameTitleInfo, service 'inGamePresence', received only by
    // same-NP-Comm-ID users. No body (pure trigger): recipients re-fetch title info via
    // GET presence / friendList. from = self, to = recipient.
    QString myComId;
    QSet<int64_t> sameComId;
    {
        // Appear-Offline: suppress outgoing presence while invisible.
        QReadLocker lk(&m_shared->clientsLock);
        auto self = m_shared->clients.constFind(m_info.userId);
        if (self != m_shared->clients.constEnd() && self->appearOffline)
            return;
    }
    {
        QReadLocker ul(&m_shared->usageLock);
        myComId = m_shared->usageClientGame.value(m_info.userId);
        if (myComId.isEmpty())
            return;
        for (auto it = m_shared->usageClientGame.cbegin(); it != m_shared->usageClientGame.cend();
             ++it) {
            if (it.value() == myComId)
                sameComId.insert(it.key());
        }
    }

    QList<QPair<QString, int64_t>> recipients; // (recipient npid, recipient userId)
    {
        QReadLocker lk(&m_shared->clientsLock);
        auto it = m_shared->clients.constFind(m_info.userId);
        if (it == m_shared->clients.constEnd())
            return;
        for (auto fr = it->friends.cbegin(); fr != it->friends.cend(); ++fr) {
            const int64_t fid = fr.key();
            if (!sameComId.contains(fid))
                continue; // SDK comId gate
            auto fit = m_shared->clients.constFind(fid);
            if (fit != m_shared->clients.constEnd())
                recipients.append({fit->npid, fid});
        }
    }

    static const QString kInGamePresence = QStringLiteral("inGamePresence");
    static const QString kGameTitleInfo = QStringLiteral("np:service:presence:gameTitleInfo");
    for (const auto& rcpt : recipients) {
        PushWebApiEvent(kInGamePresence, 0, kGameTitleInfo, QByteArray(), m_info.npid, rcpt.first,
                        rcpt.second);
    }
}

void ClientSession::PushWebApiEvent(const QString& npServiceName, quint32 npServiceLabel,
                                    const QString& dataType, const QByteArray& data,
                                    const QString& fromNpid, const QString& toNpid,
                                    int64_t targetUserId,
                                    const QList<QPair<QString, QString>>& extdData) {
    SendNotification(NotificationType::WebApiPushEvent,
                     BuildWebApiPushPayload(npServiceName, npServiceLabel, dataType, data, fromNpid,
                                            toNpid, extdData),
                     targetUserId);
}

void ClientSession::SendPacket(const QByteArray& pkt) {
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(pkt);
        m_socket->flush();
    }
}
