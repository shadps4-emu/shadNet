// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QHostAddress>
#include <QTimer>
#include <QtEndian>
#include "client_session.h"

// Serialization helpers for room data blobs

void ClientSession::AppendRoomDataInternal(QByteArray& buf, const Room& room) {
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    appendU16LE(buf, room.maxSlots); // publicSlots
    appendU16LE(buf, 0);             // privateSlots
    appendU16LE(buf,
                static_cast<uint16_t>(qMax(0, room.maxSlots - memberCount))); // openPublicSlots
    appendU16LE(buf, 0);                                                      // openPrivateSlots
    appendU16LE(buf, room.maxSlots);                                          // maxSlot
    appendU16LE(buf, room.serverId);
    appendU16LE(buf, room.worldId);
    appendU16LE(buf, room.lobbyId);
    appendU64LE(buf, room.roomId);
    appendU32LE(buf, 0); // passwdSlotMask
    appendU32LE(buf, room.joinedSlotMask());

    // Room groups
    uint16_t groupCount = room.groupConfigCount;
    appendU16LE(buf, groupCount);
    uint16_t remainingMembers = memberCount;
    for (uint16_t g = 1; g <= groupCount; ++g) {
        appendU16LE(buf, g);                                                 // groupId
        buf.append(static_cast<char>(room.roomPasswordPresent ? 1 : 0));     // hasPasswd
        buf.append(static_cast<char>(room.joinGroupLabelPresent ? 1 : 0));   // hasLabel
        buf.append(QByteArray(8, '\0'));                                     // label (8 zero bytes)
        appendU16LE(buf, g == 1 ? room.maxSlots : static_cast<uint16_t>(0)); // slots
        uint16_t gm = (g == 1) ? remainingMembers : 0;
        appendU16LE(buf, gm); // groupMembers
        remainingMembers = static_cast<uint16_t>(qMax(0, remainingMembers - gm));
    }

    appendU32LE(buf, room.flags);

    // Internal binary attributes — send actual stored data
    uint16_t actualBinAttrCount = static_cast<uint16_t>(room.binAttrsInternal.size());
    appendU16LE(buf, actualBinAttrCount);
    for (const auto& attr : room.binAttrsInternal) {
        appendU64LE(buf, 0);                                       // tick
        appendU16LE(buf, 0);                                       // memberId (room-level)
        appendU16LE(buf, attr.attrId);                             // attrId
        appendU32LE(buf, static_cast<uint32_t>(attr.data.size())); // dataSize
        buf.append(attr.data);                                     // raw data
    }
}

void ClientSession::AppendRoomDataExternal(QByteArray& buf, const Room& room) {
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    appendU16LE(buf, room.maxSlots);         // maxSlot          u16
    appendU16LE(buf, memberCount);           // curMembers       u16
    appendU32LE(buf, room.flags);            //                  u32 (OrbisNpMatching2Flags)
    appendU16LE(buf, room.serverId);         //                  u16 (OrbisNpMatching2ServerId)
    appendU32LE(buf, room.worldId);          //                  u32 (OrbisNpMatching2WorldId)
    appendU64LE(buf, room.lobbyId);          //                  u64 (OrbisNpMatching2LobbyId)
    appendU64LE(buf, room.roomId);           //                  u64 (OrbisNpMatching2RoomId)
    appendU64LE(buf, 0);                     // passwdSlotMask   u64
    appendU64LE(buf, room.joinedSlotMask()); // joinedSlotMask u64
    appendU16LE(buf, room.maxSlots);         // publicSlots      u16
    appendU16LE(buf, 0);                     // privateSlots     u16
    appendU16LE(buf,
                static_cast<uint16_t>(qMax(0, room.maxSlots - memberCount))); // openPublicSlots u16
    appendU16LE(buf, 0); // openPrivateSlots u16

    // Room groups
    uint16_t groupCount = room.groupConfigCount;
    appendU16LE(buf, groupCount);
    uint16_t remainingMembers = memberCount;
    for (uint16_t g = 1; g <= groupCount; ++g) {
        appendU16LE(buf, g);                                                 // groupId
        buf.append(static_cast<char>(room.roomPasswordPresent ? 1 : 0));     // hasPasswd
        appendU16LE(buf, g == 1 ? room.maxSlots : static_cast<uint16_t>(0)); // slots
        uint16_t gm = (g == 1) ? remainingMembers : 0;
        appendU16LE(buf, gm);
        remainingMembers = static_cast<uint16_t>(qMax(0, remainingMembers - gm));
    }

    // External search int attrs
    appendU16LE(buf, static_cast<uint16_t>(room.externalSearchIntAttrs.size()));
    for (const auto& a : room.externalSearchIntAttrs) {
        appendU16LE(buf, a.attrId);
        appendU64LE(buf, a.attrValue);
    }
    // External search bin attrs
    appendU16LE(buf, static_cast<uint16_t>(room.externalSearchBinAttrs.size()));
    for (const auto& a : room.externalSearchBinAttrs) {
        appendU16LE(buf, a.attrId);
        appendU32LE(buf, static_cast<uint32_t>(a.data.size()));
        buf.append(a.data);
    }
    // External bin attrs
    appendU16LE(buf, static_cast<uint16_t>(room.externalBinAttrs.size()));
    for (const auto& a : room.externalBinAttrs) {
        appendU16LE(buf, a.attrId);
        appendU32LE(buf, static_cast<uint32_t>(a.data.size()));
        buf.append(a.data);
    }

    // Owner NpId (game reads RoomDataExternal+0x40 as OrbisNpId* without null check)
    QByteArray ownerBytes = room.ownerNpid.toUtf8().left(16);
    ownerBytes.append(QByteArray(16 - ownerBytes.size(), '\0')); // data[16]
    buf.append(ownerBytes);
    buf.append('\0');                // term
    buf.append(QByteArray(3, '\0')); // dummy[3]
    buf.append(QByteArray(8, '\0')); // opt[8]
    buf.append(QByteArray(8, '\0')); // reserved[8]
}

void ClientSession::AppendRoomMemberDataInternal(QByteArray& buf, const Room& room,
                                                 const RoomMember& member, bool hasNext) {
    buf.append(static_cast<char>(hasNext ? 1 : 0)); // hasNext
    appendU64LE(buf, 0);                            // joinDateTicks

    // NpId: handle(onlineId(data[16] + term + dummy[3]) + opt[8] + reserved[8])
    QByteArray npidBytes = member.npid.toUtf8().left(16);
    npidBytes.append(QByteArray(16 - npidBytes.size(), '\0')); // pad to 16
    buf.append(npidBytes);
    buf.append('\0');                // term
    buf.append(QByteArray(3, '\0')); // dummy
    buf.append(QByteArray(8, '\0')); // opt
    buf.append(QByteArray(8, '\0')); // reserved

    appendU16LE(buf, member.memberId);
    appendU16LE(buf, room.teamId);
    buf.append('\0'); // natType

    uint32_t memberFlags = (member.npid == room.ownerNpid) ? 0x80000000u : 0u;
    appendU32LE(buf, memberFlags);

    // Room group (first group if any)
    bool hasGroup = room.groupConfigCount > 0;
    buf.append(static_cast<char>(hasGroup ? 1 : 0));
    if (hasGroup) {
        appendU16LE(buf, 1);                                               // groupId
        buf.append(static_cast<char>(room.roomPasswordPresent ? 1 : 0));   // hasPasswd
        buf.append(static_cast<char>(room.joinGroupLabelPresent ? 1 : 0)); // hasLabel
        buf.append(QByteArray(8, '\0'));                                   // label
        appendU16LE(buf, room.maxSlots);                                   // slots
        appendU16LE(buf, static_cast<uint16_t>(room.members.size()));      // groupMembers
    }

    // Member internal bin attrs — send actual stored data
    uint16_t actualMemberBinAttrCount = static_cast<uint16_t>(member.binAttrsInternal.size());
    appendU16LE(buf, actualMemberBinAttrCount);
    for (const auto& attr : member.binAttrsInternal) {
        appendU64LE(buf, 0); // tick
        appendU16LE(buf, attr.attrId);
        appendU32LE(buf, static_cast<uint32_t>(attr.data.size()));
        buf.append(attr.data);
    }
}

void ClientSession::AppendCreateJoinResponse(QByteArray& buf, const Room& room,
                                             const RoomMember& me) {
    // RoomDataInternal
    AppendRoomDataInternal(buf, room);

    // Member list
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    appendU16LE(buf, memberCount);

    QList<uint16_t> memberIds = room.members.keys();
    std::sort(memberIds.begin(), memberIds.end());
    for (int i = 0; i < memberIds.size(); ++i) {
        const RoomMember& m = room.members[memberIds[i]];
        bool hasNext = (i + 1 < memberIds.size());
        AppendRoomMemberDataInternal(buf, room, m, hasNext);
    }

    appendU16LE(buf, me.memberId); // meMemberId
    // Find owner memberId
    const RoomMember* owner = room.findByNpid(room.ownerNpid);
    appendU16LE(buf, owner ? owner->memberId : 0); // ownerMemberId
}

// Notification helpers

void ClientSession::SendMatchingNotification(NotificationType type, const QByteArray& payload,
                                             const QString& targetNpid) {
    std::function<void(QByteArray)> sender;
    {
        QReadLocker lk(&m_shared->clientsLock);
        auto userIdIt = m_shared->npidToUserId.find(targetNpid);
        if (userIdIt == m_shared->npidToUserId.end())
            return;
        auto clientIt = m_shared->clients.find(*userIdIt);
        if (clientIt == m_shared->clients.end())
            return;
        sender = clientIt->send;
    }
    sender(BuildNotification(type, payload));
}

void ClientSession::NotifyRoomMembers(NotificationType type, const QByteArray& payload,
                                      uint64_t roomId, const QString& excludeNpid) {
    // Collect send functions for all room members (lock ordering: roomsLock then clientsLock)
    QVector<std::function<void(QByteArray)>> senders;
    {
        QReadLocker roomLk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return;

        QReadLocker clientLk(&m_shared->clientsLock);
        for (const auto& member : roomIt->members) {
            if (member.npid == excludeNpid)
                continue;
            auto userIdIt = m_shared->npidToUserId.find(member.npid);
            if (userIdIt == m_shared->npidToUserId.end())
                continue;
            auto clientIt = m_shared->clients.find(*userIdIt);
            if (clientIt != m_shared->clients.end())
                senders.append(clientIt->send);
        }
    }
    QByteArray pkt = BuildNotification(type, payload);
    for (const auto& send : senders)
        send(pkt);
}

// Build RequestEvent notification payload

static QByteArray BuildRequestEventPayload(uint32_t ctxId, uint16_t serverId, uint16_t worldId,
                                           uint16_t lobbyId, uint16_t reqEvent, uint32_t reqId,
                                           uint32_t errorCode, uint64_t roomId, uint16_t memberId,
                                           uint16_t maxSlots, uint32_t flags, bool isOwner,
                                           const QByteArray& responseBlob) {
    QByteArray payload;
    appendU32LE(payload, ctxId);
    appendU16LE(payload, serverId);
    appendU16LE(payload, worldId);
    appendU16LE(payload, lobbyId);
    appendU16LE(payload, reqEvent);
    appendU32LE(payload, reqId);
    appendU32LE(payload, errorCode);
    appendU64LE(payload, roomId);
    appendU16LE(payload, memberId);
    appendU16LE(payload, maxSlots);
    appendU32LE(payload, flags);
    payload.append(static_cast<char>(isOwner ? 1 : 0));
    bool hasResponse = !responseBlob.isEmpty();
    payload.append(static_cast<char>(hasResponse ? 1 : 0));
    if (hasResponse)
        payload.append(responseBlob);
    return payload;
}

// Command handlers

ErrorType ClientSession::CmdRegisterHandlers(StreamExtractor& data) {
    m_matching.addr = data.getString(true);
    m_matching.port = data.get<uint16_t>();
    m_matching.ctxId = data.get<uint32_t>();
    m_matching.serviceLabel = data.get<uint32_t>();
    uint8_t handlerCount = data.get<uint8_t>();
    if (data.error())
        return ErrorType::Malformed;

    uint8_t count = qMin(handlerCount, static_cast<uint8_t>(HandlerType::Count));
    m_matching.enabledHandlersMask = 0;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t enabled = data.get<uint8_t>();
        uint64_t cbAddr = data.get<uint64_t>();
        uint64_t cbArg = data.get<uint64_t>();
        if (data.error())
            return ErrorType::Malformed;
        m_matching.callbacks[i].enabled = (enabled != 0);
        m_matching.callbacks[i].callbackAddr = cbAddr;
        m_matching.callbacks[i].callbackArg = cbArg;
        if (enabled)
            m_matching.enabledHandlersMask |= (1 << i);
    }

    // Use client's TCP peer address if no addr provided
    if (m_matching.addr.isEmpty())
        m_matching.addr = m_socket->peerAddress().toString();

    m_matching.initialized = true;

    qInfo() << "RegisterHandlers:" << m_info.npid << "addr=" << m_matching.addr
            << "port=" << m_matching.port << "ctx=" << m_matching.ctxId
            << "handlers=0x" + QString::number(m_matching.enabledHandlersMask, 16);

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdCreateRoom(StreamExtractor& data, QByteArray& reply) {
    uint32_t reqId = data.get<uint32_t>();
    uint16_t maxSlots = data.get<uint16_t>();
    uint16_t teamId = data.get<uint16_t>();
    uint16_t worldId = data.get<uint16_t>();
    uint16_t lobbyId = data.get<uint16_t>();
    uint32_t flags = data.get<uint32_t>();
    uint16_t groupConfigCount = data.get<uint16_t>();
    uint16_t allowedUserCount = data.get<uint16_t>();
    uint16_t blockedUserCount = data.get<uint16_t>();
    uint16_t internalBinAttrCount = data.get<uint16_t>();
    uint16_t externalSearchIntAttrCount = data.get<uint16_t>();
    QVector<Room::IntAttrEntry> createSearchIntAttrs;
    for (uint16_t i = 0; i < externalSearchIntAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        uint32_t attrVal = data.get<uint32_t>();
        if (data.error())
            return ErrorType::Malformed;
        createSearchIntAttrs.append({attrId, attrVal});
    }
    uint16_t externalSearchBinAttrCount = data.get<uint16_t>();
    QVector<RoomBinAttrEntry> createSearchBinAttrs;
    for (uint16_t i = 0; i < externalSearchBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        createSearchBinAttrs.append({attrId, attrData});
    }
    uint16_t externalBinAttrCount = data.get<uint16_t>();
    QVector<RoomBinAttrEntry> createExtBinAttrs;
    for (uint16_t i = 0; i < externalBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        createExtBinAttrs.append({attrId, attrData});
    }
    uint16_t memberInternalBinAttrCount = data.get<uint16_t>();
    // Parse member bin attr data for the creating member
    QVector<RoomBinAttrEntry> creatorMemberBinAttrs;
    for (uint16_t i = 0; i < memberInternalBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        creatorMemberBinAttrs.append({attrId, attrData});
    }
    uint8_t joinGroupLabelPresent = data.get<uint8_t>();
    uint8_t roomPasswordPresent = data.get<uint8_t>();
    uint8_t sigType = data.get<uint8_t>();
    uint8_t sigFlag = data.get<uint8_t>();
    uint16_t sigMainMember = data.get<uint16_t>();
    if (data.error())
        return ErrorType::Malformed;

    if (maxSlots < 1)
        maxSlots = 1;

    uint64_t rid = m_shared->matching.nextRoomId.fetch_add(1);

    Room room;
    room.roomId = rid;
    room.maxSlots = maxSlots;
    room.ownerNpid = m_info.npid;
    room.serverId = m_matching.serverId;
    room.worldId = worldId ? worldId : m_matching.worldId;
    room.lobbyId = lobbyId ? lobbyId : m_matching.lobbyId;
    room.flags = flags;
    room.teamId = teamId;
    room.groupConfigCount = groupConfigCount;
    room.allowedUserCount = allowedUserCount;
    room.blockedUserCount = blockedUserCount;
    room.internalBinAttrCount = internalBinAttrCount;
    room.externalSearchIntAttrCount = externalSearchIntAttrCount;
    room.externalSearchBinAttrCount = externalSearchBinAttrCount;
    room.externalBinAttrCount = externalBinAttrCount;
    room.externalSearchIntAttrs = createSearchIntAttrs;
    room.externalSearchBinAttrs = createSearchBinAttrs;
    room.externalBinAttrs = createExtBinAttrs;
    room.memberInternalBinAttrCount = memberInternalBinAttrCount;
    room.joinGroupLabelPresent = (joinGroupLabelPresent != 0);
    room.roomPasswordPresent = (roomPasswordPresent != 0);
    room.signalingType = sigType;
    room.signalingFlag = sigFlag;
    room.signalingMainMember = sigMainMember;

    RoomMember* member = room.addMember(m_info.npid, m_matching.addr, m_matching.port);
    member->binAttrsInternal = creatorMemberBinAttrs;
    uint16_t memberId = member->memberId;

    // Update session state
    m_matching.roomId = rid;
    m_matching.myMemberId = memberId;
    m_matching.isRoomOwner = true;
    m_matching.maxSlots = maxSlots;
    m_matching.roomFlags = flags;
    m_matching.serverId = room.serverId;
    m_matching.worldId = room.worldId;
    m_matching.lobbyId = room.lobbyId;

    // Build reply
    appendU64LE(reply, rid);
    appendU16LE(reply, room.serverId);
    appendU16LE(reply, room.worldId);
    appendU16LE(reply, room.lobbyId);
    appendU16LE(reply, memberId);
    appendU16LE(reply, maxSlots);
    appendU32LE(reply, flags);
    appendU16LE(reply, 1); // curMemberNum
    AppendCreateJoinResponse(reply, room, *member);

    // Push RequestEvent (0x0101 CreateJoinRoom) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray responseBlob;
        AppendCreateJoinResponse(responseBlob, room, *member);
        QByteArray reqPayload = BuildRequestEventPayload(
            m_matching.ctxId, room.serverId, room.worldId, room.lobbyId, 0x0101, reqId, 0, rid,
            memberId, maxSlots, flags, true, responseBlob);
        SendSelfNotification(NotificationType::RequestEvent, reqPayload);
    }

    // Store room (after building reply since we need the member pointer)
    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        m_shared->matching.rooms.insert(rid, std::move(room));
    }

    qInfo() << "Room" << rid << "created by" << m_info.npid << "max=" << maxSlots;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdJoinRoom(StreamExtractor& data, QByteArray& reply) {
    uint64_t roomId = data.get<uint64_t>();
    uint32_t reqId = data.get<uint32_t>();
    // Read optional join settings
    uint16_t teamId = data.get<uint16_t>();
    uint32_t joinFlags = data.get<uint32_t>();
    data.get<uint16_t>(); // blockedUserCount
    uint16_t joinMemberBinAttrCount = data.get<uint16_t>();
    QVector<RoomBinAttrEntry> joinerMemberBinAttrs;
    for (uint16_t i = 0; i < joinMemberBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        joinerMemberBinAttrs.append({attrId, attrData});
    }
    data.get<uint8_t>(); // roomPasswordPresent
    data.get<uint8_t>(); // joinGroupLabelPresent
    if (data.error())
        return ErrorType::Malformed;
    Q_UNUSED(teamId);
    Q_UNUSED(joinFlags);

    // Snapshot of existing members for notifications after lock release
    struct MemberSnapshot {
        QString npid;
        uint16_t memberId;
        QString addr;
        uint16_t port;
    };
    QVector<MemberSnapshot> existingMembers;
    uint16_t myMemberId = 0;
    uint16_t maxSlots = 0;
    uint32_t flags = 0;
    uint16_t curMemberNum = 0;
    QByteArray responseBlob;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();
        if (room.isFull())
            return ErrorType::RoomFull;
        if (room.findByNpid(m_info.npid))
            return ErrorType::RoomAlreadyJoined;

        // Snapshot existing members
        for (const auto& m : room.members)
            existingMembers.append({m.npid, m.memberId, m.addr, m.port});

        // Add self
        RoomMember* member = room.addMember(m_info.npid, m_matching.addr, m_matching.port);
        member->binAttrsInternal = joinerMemberBinAttrs;
        myMemberId = member->memberId;
        maxSlots = room.maxSlots;
        flags = room.flags;
        curMemberNum = static_cast<uint16_t>(room.members.size());

        // Update session
        m_matching.roomId = roomId;
        m_matching.myMemberId = myMemberId;
        m_matching.isRoomOwner = false;
        m_matching.maxSlots = maxSlots;
        m_matching.roomFlags = flags;
        m_matching.serverId = room.serverId;
        m_matching.worldId = room.worldId;
        m_matching.lobbyId = room.lobbyId;

        // Build reply
        appendU64LE(reply, roomId);
        appendU16LE(reply, myMemberId);
        appendU16LE(reply, maxSlots);
        appendU32LE(reply, flags);
        appendU16LE(reply, curMemberNum);
        AppendCreateJoinResponse(reply, room, *member);

        // Build response blob for RequestEvent
        if (m_matching.hasHandler(HandlerType::Request))
            AppendCreateJoinResponse(responseBlob, room, *member);
    }

    qInfo() << "Room" << roomId << m_info.npid << "joined mid=" << myMemberId;

    // Push MemberJoined (0x1101) to existing members with room_event handler
    {
        QByteArray payload;
        appendU64LE(payload, roomId);
        appendU16LE(payload, myMemberId);
        appendCStr(payload, m_info.npid);
        appendCStr(payload, m_matching.addr);
        appendU16LE(payload, m_matching.port);
        // Include joining member's bin attrs so host can display them
        appendU16LE(payload, static_cast<uint16_t>(joinerMemberBinAttrs.size()));
        for (const auto& a : joinerMemberBinAttrs) {
            appendU16LE(payload, a.attrId);
            appendU32LE(payload, static_cast<uint32_t>(a.data.size()));
            payload.append(a.data);
        }
        NotifyRoomMembers(NotificationType::MemberJoined, payload, roomId, m_info.npid);
    }

    // Exchange SignalingHelper with existing members
    for (const auto& em : existingMembers) {
        // Send existing member's info to joining client
        if (m_matching.hasHandler(HandlerType::Signaling)) {
            QByteArray payload;
            appendCStr(payload, em.npid);
            appendU16LE(payload, em.memberId);
            appendCStr(payload, em.addr);
            appendU16LE(payload, em.port);
            SendSelfNotification(NotificationType::SignalingHelper, payload);
        }
        // Send joining client's info to existing member
        {
            QByteArray payload;
            appendCStr(payload, m_info.npid);
            appendU16LE(payload, myMemberId);
            appendCStr(payload, m_matching.addr);
            appendU16LE(payload, m_matching.port);
            SendMatchingNotification(NotificationType::SignalingHelper, payload, em.npid);
        }
    }

    // Push RequestEvent (0x0102 JoinRoom) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray reqPayload = BuildRequestEventPayload(
            m_matching.ctxId, m_matching.serverId, m_matching.worldId, m_matching.lobbyId, 0x0102,
            reqId, 0, roomId, myMemberId, maxSlots, flags, false, responseBlob);
        SendSelfNotification(NotificationType::RequestEvent, reqPayload);
    }

    // Send current room bin attrs to the joining client so the game has them.
    // This arrives as a 0x1106 RoomDataInternalUpdated event AFTER the 0x0102 callback.
    if (m_matching.hasHandler(HandlerType::RoomEvent)) {
        QVector<RoomBinAttrEntry> binSnapshot;
        uint32_t roomFlags = 0;
        {
            QReadLocker lk(&m_shared->matching.roomsLock);
            auto roomIt = m_shared->matching.rooms.find(roomId);
            if (roomIt != m_shared->matching.rooms.end()) {
                binSnapshot = roomIt->binAttrsInternal;
                roomFlags = roomIt->flags;
            }
        }
        if (!binSnapshot.isEmpty()) {
            QByteArray binPayload;
            appendU64LE(binPayload, roomId);
            appendU32LE(binPayload, roomFlags);
            appendU16LE(binPayload, static_cast<uint16_t>(binSnapshot.size()));
            for (const auto& a : binSnapshot) {
                appendU16LE(binPayload, a.attrId);
                appendU32LE(binPayload, static_cast<uint32_t>(a.data.size()));
                binPayload.append(a.data);
            }
            SendSelfNotification(NotificationType::RoomDataInternalUpdated, binPayload);
        }
    }

    // Delayed ESTABLISHED push (2 seconds)
    // Value-capture everything needed — room state may change during delay
    QString myNpid = m_info.npid;
    bool myHasSignaling = m_matching.hasHandler(HandlerType::Signaling);
    SharedState* shared = m_shared;

    QTimer::singleShot(2000, this, [=]() {
        // Push SignalingEvent(0x5102) to each existing member for the joining member
        for (const auto& em : existingMembers) {
            QByteArray payload;
            appendU16LE(payload, 0x5102); // ESTABLISHED
            appendU64LE(payload, roomId);
            appendU16LE(payload, myMemberId);
            appendU32LE(payload, myMemberId); // connId = memberId
            SendMatchingNotification(NotificationType::SignalingEvent, payload, em.npid);
        }
        // Push SignalingEvent(0x5102) to joining member for each existing member
        if (myHasSignaling) {
            for (const auto& em : existingMembers) {
                QByteArray payload;
                appendU16LE(payload, 0x5102);
                appendU64LE(payload, roomId);
                appendU16LE(payload, em.memberId);
                appendU32LE(payload, em.memberId);
                SendSelfNotification(NotificationType::SignalingEvent, payload);
            }
        }
    });

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdLeaveRoom(StreamExtractor& data, QByteArray& reply) {
    uint64_t roomId = data.get<uint64_t>();
    uint32_t reqId = data.get<uint32_t>();
    if (data.error())
        return ErrorType::Malformed;

    struct MemberSnapshot {
        QString npid;
        uint16_t memberId;
    };
    QVector<MemberSnapshot> remainingMembers;
    uint16_t myMemberId = 0;
    bool wasOwner = false;
    bool roomDestroyed = false;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        const RoomMember* me = room.findByNpid(m_info.npid);
        if (!me)
            return ErrorType::RoomMissing;
        myMemberId = me->memberId;
        wasOwner = (m_info.npid == room.ownerNpid);

        room.removeMember(myMemberId);

        if (room.isEmpty()) {
            m_shared->matching.rooms.erase(roomIt);
            roomDestroyed = true;
            qInfo() << "Room" << roomId << "destroyed";
        } else {
            // Transfer ownership if needed
            if (wasOwner) {
                room.ownerNpid = room.members.begin()->npid;
                qInfo() << "Room" << roomId << "owner ->" << room.ownerNpid;
            }
            for (const auto& m : room.members)
                remainingMembers.append({m.npid, m.memberId});
        }
    }

    // Clear session room state
    m_matching.roomId = 0;
    m_matching.myMemberId = 0;
    m_matching.isRoomOwner = false;
    m_matching.maxSlots = 0;
    m_matching.roomFlags = 0;

    // Clear stale signaling pairs
    {
        QWriteLocker lk(&m_shared->matching.signalingLock);
        auto& pairs = m_shared->matching.signalingPairs;
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it.key().first == m_info.npid || it.key().second == m_info.npid)
                it = pairs.erase(it);
            else
                ++it;
        }
    }
    // Clear stale activation intents
    {
        QWriteLocker lk(&m_shared->matching.activationLock);
        auto& intents = m_shared->matching.activationIntents;
        for (auto it = intents.begin(); it != intents.end();) {
            if (it.value().first == m_info.npid || it.value().second == m_info.npid)
                it = intents.erase(it);
            else
                ++it;
        }
    }

    qInfo() << "Room" << roomId << m_info.npid << "left mid=" << myMemberId;

    // Notify remaining members (MemberLeft 0x1102)
    if (!roomDestroyed) {
        QByteArray memberLeftPayload;
        appendU64LE(memberLeftPayload, roomId);
        appendU16LE(memberLeftPayload, myMemberId);
        appendCStr(memberLeftPayload, m_info.npid);
        NotifyRoomMembers(NotificationType::MemberLeft, memberLeftPayload, roomId);
    }

    // Push RequestEvent (0x0103 LeaveRoom) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        // LeaveRoom response blob is just the roomId
        QByteArray leaveResponseBlob;
        appendU64LE(leaveResponseBlob, roomId);
        QByteArray reqPayload = BuildRequestEventPayload(
            m_matching.ctxId, m_matching.serverId, m_matching.worldId, m_matching.lobbyId, 0x0103,
            reqId, 0, roomId, myMemberId, 0, 0, false, leaveResponseBlob);
        SendSelfNotification(NotificationType::RequestEvent, reqPayload);
    }

    // Reply
    appendU64LE(reply, roomId);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetRoomList(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(data);

    QReadLocker lk(&m_shared->matching.roomsLock);
    uint32_t roomCount = static_cast<uint32_t>(m_shared->matching.rooms.size());
    appendU32LE(reply, roomCount);
    for (const auto& room : m_shared->matching.rooms) {
        AppendRoomDataExternal(reply, room);
        qInfo() << "GetRoomList: room" << room.roomId << "owner=" << room.ownerNpid
                << "members=" << room.members.size() << "maxSlots=" << room.maxSlots
                << "flags=" << Qt::hex << room.flags;
    }

    qInfo() << "GetRoomList:" << m_info.npid << "— returning" << roomCount << "rooms";
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRequestSignalingInfos(StreamExtractor& data, QByteArray& reply) {
    QString targetNpid = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    // Resolve target's external UDP endpoint
    QString targetIp;
    uint16_t targetPort = 0;
    {
        QReadLocker lk(&m_shared->matching.udpLock);
        auto it = m_shared->matching.udpExt.find(targetNpid);
        if (it != m_shared->matching.udpExt.end()) {
            targetIp = it->first;
            targetPort = it->second;
        }
    }
    // Fallback to session addr/port if no STUN data
    if (targetIp.isEmpty()) {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (const auto& room : m_shared->matching.rooms) {
            const RoomMember* tm = room.findByNpid(targetNpid);
            if (tm) {
                targetIp = tm->addr;
                targetPort = tm->port;
                break;
            }
        }
    }
    if (targetIp.isEmpty())
        return ErrorType::NotFound;

    // Resolve requester's external endpoint
    QString myIp;
    uint16_t myPort = 0;
    {
        QReadLocker lk(&m_shared->matching.udpLock);
        auto it = m_shared->matching.udpExt.find(m_info.npid);
        if (it != m_shared->matching.udpExt.end()) {
            myIp = it->first;
            myPort = it->second;
        }
    }
    if (myIp.isEmpty()) {
        myIp = m_matching.addr;
        myPort = m_matching.port;
    }

    // Find member IDs via shared room
    uint16_t targetMemberId = 0;
    uint16_t myMemberId = 0;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (const auto& room : m_shared->matching.rooms) {
            const RoomMember* tm = room.findByNpid(targetNpid);
            const RoomMember* mm = room.findByNpid(m_info.npid);
            if (tm && mm) {
                targetMemberId = tm->memberId;
                myMemberId = mm->memberId;
                break;
            }
        }
    }

    qInfo() << "RequestSignalingInfos:" << m_info.npid << "(mid=" << myMemberId << ") ->"
            << targetNpid << "(mid=" << targetMemberId << ") target=" << targetIp << ":"
            << targetPort;

    // Reply with target's endpoint
    appendCStr(reply, targetNpid);
    appendCStr(reply, targetIp);
    appendU16LE(reply, targetPort);
    appendU16LE(reply, targetMemberId);

    // Push SignalingHelper to target with requester's info
    {
        QByteArray payload;
        appendCStr(payload, m_info.npid);
        appendU16LE(payload, myMemberId);
        appendCStr(payload, myIp);
        appendU16LE(payload, myPort);
        SendMatchingNotification(NotificationType::SignalingHelper, payload, targetNpid);
    }

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSignalingEstablished(StreamExtractor& data) {
    QString targetNpid = data.getString(false);
    uint32_t connId = data.get<uint32_t>();
    if (data.error())
        return ErrorType::Malformed;

    // Find shared room for logging
    uint16_t myMemberId = 0;
    uint16_t targetMemberId = 0;
    uint64_t roomId = 0;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (auto it = m_shared->matching.rooms.begin(); it != m_shared->matching.rooms.end();
             ++it) {
            const RoomMember* mm = it->findByNpid(m_info.npid);
            const RoomMember* tm = it->findByNpid(targetNpid);
            if (mm && tm) {
                myMemberId = mm->memberId;
                targetMemberId = tm->memberId;
                roomId = it.key();
                break;
            }
        }
    }

    qInfo() << "SignalingEstablished:" << m_info.npid << "(mid=" << myMemberId << ") <->"
            << targetNpid << "(mid=" << targetMemberId << ") room=" << roomId << "conn=" << connId;

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdActivationConfirm(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(reply);
    QString meId = data.getString(false);
    QString initiatorIpStr = data.getString(false);
    uint32_t ctxTag = data.get<uint32_t>();
    if (data.error())
        return ErrorType::Malformed;

    // Convert dotted IP to network-order u32
    QHostAddress addr(initiatorIpStr);
    if (addr.isNull())
        return ErrorType::InvalidInput;
    uint32_t initiatorIpInt = qToBigEndian(static_cast<uint32_t>(addr.toIPv4Address()));

    QPair<uint32_t, uint32_t> key(initiatorIpInt, ctxTag);
    qInfo() << "ActivationConfirm TCP: me=" << meId << "initiator_ip=" << initiatorIpStr
            << "ctx_tag=0x" + QString::number(ctxTag, 16);

    QString initiatorNpid;
    {
        QReadLocker lk(&m_shared->matching.activationLock);
        auto it = m_shared->matching.activationIntents.find(key);
        if (it == m_shared->matching.activationIntents.end()) {
            qWarning() << "ActivationConfirm: no intent for key — replying error";
            return ErrorType::NotFound;
        }
        initiatorNpid = it->first;
    }

    qInfo() << "ActivationConfirm: matched intent initiator=" << initiatorNpid
            << "— sending NpSignalingEvent event=1, peer=" << meId;

    // Fire NpSignalingEvent{event:1} to the initiator
    QByteArray payload;
    appendU32LE(payload, 1); // event = connection activated
    appendCStr(payload, meId);
    SendMatchingNotification(NotificationType::NpSignalingEvent, payload, initiatorNpid);

    return ErrorType::NoError;
}

// SetRoomDataInternal

ErrorType ClientSession::CmdSetRoomDataInternal(StreamExtractor& data, QByteArray& reply) {
    uint32_t reqId = data.get<uint32_t>();
    uint64_t roomId = data.get<uint64_t>();
    uint32_t flagFilter = data.get<uint32_t>();
    uint32_t flagAttr = data.get<uint32_t>();
    uint16_t binAttrCount = data.get<uint16_t>();
    if (data.error())
        return ErrorType::Malformed;

    struct BinAttr {
        uint16_t attrId;
        QByteArray attrData;
    };
    QVector<BinAttr> attrs;
    attrs.reserve(binAttrCount);
    for (uint16_t i = 0; i < binAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        // getRawData() reads a u32 size then that many bytes — matches our wire format
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        attrs.append({attrId, attrData});
    }

    uint8_t hasPasswdMask = data.get<uint8_t>();
    uint64_t passwdSlotMask = 0;
    if (hasPasswdMask)
        passwdSlotMask = data.get<uint64_t>();
    if (data.error())
        return ErrorType::Malformed;

    uint32_t newFlags = 0;
    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        // Apply flag filter/attr: clear bits in filter, then set bits from attr
        room.flags = (room.flags & ~flagFilter) | (flagAttr & flagFilter);
        newFlags = room.flags;

        if (hasPasswdMask)
            room.passwdSlotMask = passwdSlotMask;

        // Update bin attrs (replace by id, insert if new)
        for (const auto& a : attrs) {
            bool found = false;
            for (auto& existing : room.binAttrsInternal) {
                if (existing.attrId == a.attrId) {
                    existing.data = a.attrData;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.binAttrsInternal.append({a.attrId, a.attrData});
        }

        qInfo() << "SetRoomDataInternal: room=" << roomId << "flags=" << Qt::hex << newFlags
                << "binAttrs=" << attrs.size();
    }

    // Push RequestEvent(0x0109) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray responseBlob;
        appendU64LE(responseBlob, roomId);
        QByteArray reqPayload = BuildRequestEventPayload(
            m_matching.ctxId, m_matching.serverId, m_matching.worldId, m_matching.lobbyId, 0x0109,
            reqId, 0, roomId, m_matching.myMemberId, m_matching.maxSlots, newFlags,
            m_matching.isRoomOwner, responseBlob);
        SendSelfNotification(NotificationType::RequestEvent, reqPayload);
    }

    // Broadcast RoomDataInternalUpdated to all other members
    QByteArray notifPayload;
    appendU64LE(notifPayload, roomId);
    appendU32LE(notifPayload, newFlags);
    appendU16LE(notifPayload, static_cast<uint16_t>(attrs.size()));
    for (const auto& a : attrs) {
        appendU16LE(notifPayload, a.attrId);
        appendU32LE(notifPayload, static_cast<uint32_t>(a.attrData.size()));
        notifPayload.append(a.attrData);
    }
    NotifyRoomMembers(NotificationType::RoomDataInternalUpdated, notifPayload, roomId, m_info.npid);

    appendU64LE(reply, roomId);
    return ErrorType::NoError;
}

// SetRoomDataExternal

ErrorType ClientSession::CmdSetRoomDataExternal(StreamExtractor& data, QByteArray& reply) {
    uint32_t reqId = data.get<uint32_t>();
    uint64_t roomId = data.get<uint64_t>();

    // Searchable int attrs
    uint16_t intAttrCount = data.get<uint16_t>();
    if (data.error())
        return ErrorType::Malformed;
    QVector<Room::IntAttrEntry> intAttrs;
    intAttrs.reserve(intAttrCount);
    for (uint16_t i = 0; i < intAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        uint32_t attrValue = data.get<uint32_t>();
        if (data.error())
            return ErrorType::Malformed;
        intAttrs.append({attrId, attrValue});
    }

    // Searchable bin attrs
    uint16_t searchBinAttrCount = data.get<uint16_t>();
    if (data.error())
        return ErrorType::Malformed;
    QVector<RoomBinAttrEntry> searchBinAttrs;
    searchBinAttrs.reserve(searchBinAttrCount);
    for (uint16_t i = 0; i < searchBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        searchBinAttrs.append({attrId, attrData});
    }

    // External bin attrs
    uint16_t extBinAttrCount = data.get<uint16_t>();
    if (data.error())
        return ErrorType::Malformed;
    QVector<RoomBinAttrEntry> extBinAttrs;
    extBinAttrs.reserve(extBinAttrCount);
    for (uint16_t i = 0; i < extBinAttrCount; ++i) {
        uint16_t attrId = data.get<uint16_t>();
        QByteArray attrData = data.getRawData();
        if (data.error())
            return ErrorType::Malformed;
        extBinAttrs.append({attrId, attrData});
    }

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        // Merge by ID: update existing, insert new
        for (const auto& a : intAttrs) {
            bool found = false;
            for (auto& existing : room.externalSearchIntAttrs) {
                if (existing.attrId == a.attrId) {
                    existing.attrValue = a.attrValue;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalSearchIntAttrs.append(a);
        }
        for (const auto& a : searchBinAttrs) {
            bool found = false;
            for (auto& existing : room.externalSearchBinAttrs) {
                if (existing.attrId == a.attrId) {
                    existing.data = a.data;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalSearchBinAttrs.append(a);
        }
        for (const auto& a : extBinAttrs) {
            bool found = false;
            for (auto& existing : room.externalBinAttrs) {
                if (existing.attrId == a.attrId) {
                    existing.data = a.data;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalBinAttrs.append(a);
        }

        qInfo() << "SetRoomDataExternal: room=" << roomId << "searchIntAttrs=" << intAttrs.size()
                << "searchBinAttrs=" << searchBinAttrs.size()
                << "extBinAttrs=" << extBinAttrs.size();
    }

    // Push RequestEvent(0x0004) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray responseBlob;
        appendU64LE(responseBlob, roomId);
        QByteArray reqPayload = BuildRequestEventPayload(
            m_matching.ctxId, m_matching.serverId, m_matching.worldId, m_matching.lobbyId, 0x0004,
            reqId, 0, roomId, m_matching.myMemberId, m_matching.maxSlots, m_matching.roomFlags,
            m_matching.isRoomOwner, responseBlob);
        SendSelfNotification(NotificationType::RequestEvent, reqPayload);
    }

    appendU64LE(reply, roomId);
    return ErrorType::NoError;
}

// DoLeaveRoom (reusable for both CmdLeaveRoom and disconnect cleanup)

void ClientSession::DoLeaveRoom(uint64_t roomId) {
    struct MemberSnapshot {
        QString npid;
        uint16_t memberId;
    };
    QVector<MemberSnapshot> remainingMembers;
    uint16_t myMemberId = 0;
    bool roomDestroyed = false;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return;
        Room& room = roomIt.value();

        const RoomMember* me = room.findByNpid(m_info.npid);
        if (!me)
            return;
        myMemberId = me->memberId;
        bool wasOwner = (m_info.npid == room.ownerNpid);

        room.removeMember(myMemberId);

        if (room.isEmpty()) {
            m_shared->matching.rooms.erase(roomIt);
            roomDestroyed = true;
            qInfo() << "Room" << roomId << "destroyed (disconnect)";
        } else {
            if (wasOwner) {
                room.ownerNpid = room.members.begin()->npid;
                qInfo() << "Room" << roomId << "owner ->" << room.ownerNpid << "(disconnect)";
            }
            for (const auto& m : room.members)
                remainingMembers.append({m.npid, m.memberId});
        }
    }

    // Clear session room state
    m_matching.roomId = 0;
    m_matching.myMemberId = 0;
    m_matching.isRoomOwner = false;

    // Notify remaining members
    if (!roomDestroyed) {
        QByteArray payload;
        appendU64LE(payload, roomId);
        appendU16LE(payload, myMemberId);
        appendCStr(payload, m_info.npid);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, roomId);
    }
}

// Disconnect cleanup

void ClientSession::CleanupMatchingOnDisconnect() {
    if (!m_matching.initialized)
        return;

    // Leave any room
    if (m_matching.roomId != 0)
        DoLeaveRoom(m_matching.roomId);

    // Clear stale signaling pairs
    {
        QWriteLocker lk(&m_shared->matching.signalingLock);
        auto& pairs = m_shared->matching.signalingPairs;
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it.key().first == m_info.npid || it.key().second == m_info.npid)
                it = pairs.erase(it);
            else
                ++it;
        }
    }
    // Clear stale activation intents
    {
        QWriteLocker lk(&m_shared->matching.activationLock);
        auto& intents = m_shared->matching.activationIntents;
        for (auto it = intents.begin(); it != intents.end();) {
            if (it.value().first == m_info.npid || it.value().second == m_info.npid)
                it = intents.erase(it);
            else
                ++it;
        }
    }

    qInfo() << "Matching cleanup for" << m_info.npid;
}
