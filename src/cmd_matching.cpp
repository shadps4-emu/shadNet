// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QHostAddress>
#include <QTimer>
#include <QtEndian>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

namespace {

constexpr int32_t MATCHING2_KICKEDOUT_STATUS_CODE = static_cast<int32_t>(0xFF000019u);
constexpr uint32_t MATCHING2_KICKEDOUT_GUARD_VALUE = 4;

void ResetLocalMatchingRoomState(MatchingSessionState& matching) {
    matching.roomId = 0;
    matching.myMemberId = 0;
    matching.isRoomOwner = false;
    matching.maxSlots = 0;
    matching.roomFlags = 0;
}

void ClearPeerMatchingArtifacts(SharedState* shared, const QString& npid) {
    {
        QWriteLocker lk(&shared->matching.signalingLock);
        auto& pairs = shared->matching.signalingPairs;
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it.key().first == npid || it.key().second == npid)
                it = pairs.erase(it);
            else
                ++it;
        }
    }
    {
        QWriteLocker lk(&shared->matching.activationLock);
        auto& intents = shared->matching.activationIntents;
        for (auto it = intents.begin(); it != intents.end();) {
            if (it.value().first == npid || it.value().second == npid)
                it = intents.erase(it);
            else
                ++it;
        }
    }
}

} // namespace

// ── Proto builder helpers ─────────────────────────────────────────────────────

static void FillRoomGroup(shadnet::MatchingRoomGroup* grp, const Room& room, uint16_t g,
                          uint16_t memberCount) {
    grp->set_group_id(g);
    grp->set_has_passwd(room.roomPasswordPresent);
    grp->set_has_label(room.joinGroupLabelPresent);
    grp->set_slot_count(g == 1 ? room.maxSlots : 0u);
    grp->set_num_members(g == 1 ? memberCount : 0u);
}

static shadnet::MatchingRoomDataInternal BuildRoomDataInternal(const Room& room) {
    shadnet::MatchingRoomDataInternal pb;
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    pb.set_public_slots(room.maxSlots);
    pb.set_open_public_slots(static_cast<uint32_t>(qMax(0, room.maxSlots - memberCount)));
    pb.set_max_slot(room.maxSlots);
    pb.set_server_id(room.serverId);
    pb.set_world_id(room.worldId);
    pb.set_lobby_id(room.lobbyId);
    pb.set_room_id(room.roomId);
    pb.set_joined_slot_mask(room.joinedSlotMask());
    for (uint16_t g = 1; g <= room.groupConfigCount; ++g)
        FillRoomGroup(pb.add_groups(), room, g, memberCount);
    pb.set_flags(room.flags);
    for (const auto& attr : room.binAttrsInternal) {
        auto* a = pb.add_bin_attrs_internal();
        a->set_attr_id(attr.attrId);
        a->set_data(attr.data.constData(), attr.data.size());
    }
    return pb;
}

static shadnet::MatchingRoomMemberData BuildRoomMemberData(const Room& room,
                                                           const RoomMember& member) {
    shadnet::MatchingRoomMemberData pb;
    pb.set_npid(member.npid.toStdString());
    pb.set_member_id(member.memberId);
    pb.set_team_id(room.teamId);
    pb.set_is_owner(member.npid == room.ownerNpid);
    if (room.groupConfigCount > 0) {
        uint16_t memberCount = static_cast<uint16_t>(room.members.size());
        FillRoomGroup(pb.mutable_group(), room, 1, memberCount);
    }
    for (const auto& attr : member.binAttrsInternal) {
        auto* a = pb.add_bin_attrs_internal();
        a->set_attr_id(attr.attrId);
        a->set_data(attr.data.constData(), attr.data.size());
    }
    return pb;
}

static shadnet::CreateJoinRoomResponse BuildCreateJoinResponse(const Room& room,
                                                               const RoomMember& me) {
    shadnet::CreateJoinRoomResponse pb;
    *pb.mutable_room_data() = BuildRoomDataInternal(room);
    QList<uint16_t> memberIds = room.members.keys();
    std::sort(memberIds.begin(), memberIds.end());
    for (uint16_t mid : memberIds)
        *pb.add_members() = BuildRoomMemberData(room, room.members[mid]);
    pb.set_me_member_id(me.memberId);
    const RoomMember* owner = room.findByNpid(room.ownerNpid);
    pb.set_owner_member_id(owner ? owner->memberId : 0u);
    return pb;
}

static shadnet::MatchingRoomDataExternal BuildRoomDataExternal(const Room& room) {
    shadnet::MatchingRoomDataExternal pb;
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    pb.set_max_slot(room.maxSlots);
    pb.set_cur_members(memberCount);
    pb.set_flags(room.flags);
    pb.set_server_id(room.serverId);
    pb.set_world_id(room.worldId);
    pb.set_lobby_id(room.lobbyId);
    pb.set_room_id(room.roomId);
    pb.set_passwd_slot_mask(room.passwdSlotMask);
    pb.set_joined_slot_mask(room.joinedSlotMask());
    pb.set_public_slots(room.maxSlots);
    pb.set_open_public_slots(static_cast<uint32_t>(qMax(0, room.maxSlots - memberCount)));
    for (uint16_t g = 1; g <= room.groupConfigCount; ++g)
        FillRoomGroup(pb.add_groups(), room, g, memberCount);
    for (const auto& a : room.externalSearchIntAttrs) {
        auto* ia = pb.add_external_search_int_attrs();
        ia->set_attr_id(a.attrId);
        ia->set_attr_value(a.attrValue);
    }
    for (const auto& a : room.externalSearchBinAttrs) {
        auto* ba = pb.add_external_search_bin_attrs();
        ba->set_attr_id(a.attrId);
        ba->set_data(a.data.constData(), a.data.size());
    }
    for (const auto& a : room.externalBinAttrs) {
        auto* ba = pb.add_external_bin_attrs();
        ba->set_attr_id(a.attrId);
        ba->set_data(a.data.constData(), a.data.size());
    }
    pb.set_owner_npid(room.ownerNpid.toStdString());
    return pb;
}

// Build a fully-encoded NotifyRequestEvent payload (u32-LE prefix + proto bytes).
static QByteArray MakeRequestEventPayload(uint32_t ctxId, uint16_t serverId, uint16_t worldId,
                                          uint16_t lobbyId, uint32_t reqEvent, uint32_t reqId,
                                          uint32_t errorCode, uint64_t roomId, uint16_t memberId,
                                          uint16_t maxSlots, uint32_t flags, bool isOwner,
                                          const QByteArray& responseBlob) {
    shadnet::NotifyRequestEvent pb;
    pb.set_ctx_id(ctxId);
    pb.set_server_id(serverId);
    pb.set_world_id(worldId);
    pb.set_lobby_id(lobbyId);
    pb.set_req_event(reqEvent);
    pb.set_req_id(reqId);
    pb.set_error_code(errorCode);
    pb.set_room_id(roomId);
    pb.set_member_id(memberId);
    pb.set_max_slots(maxSlots);
    pb.set_flags(flags);
    pb.set_is_owner(isOwner);
    if (!responseBlob.isEmpty())
        pb.set_response_blob(responseBlob.constData(), responseBlob.size());
    QByteArray payload;
    appendProto(payload, pb);
    return payload;
}

// ── Notification helpers ──────────────────────────────────────────────────────

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

// ── Command handlers ──────────────────────────────────────────────────────────

void ClientSession::ResetMatchingRoomState(uint64_t roomId) {
    if (roomId == 0 || m_matching.roomId != roomId)
        return;
    ResetLocalMatchingRoomState(m_matching);
    ClearPeerMatchingArtifacts(m_shared, m_info.npid);
    qInfo() << "Matching room state reset for" << m_info.npid << "room=" << roomId;
}

ErrorType ClientSession::CmdRegisterHandlers(StreamExtractor& data) {
    shadnet::RegisterHandlersRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    m_matching.addr = QString::fromStdString(req.addr());
    m_matching.port = static_cast<uint16_t>(req.port());
    m_matching.ctxId = req.ctx_id();
    m_matching.serviceLabel = req.service_label();

    int count = qMin(req.callbacks_size(), static_cast<int>(HandlerType::Count));
    m_matching.enabledHandlersMask = 0;
    for (int i = 0; i < count; ++i) {
        const auto& cb = req.callbacks(i);
        m_matching.callbacks[i].enabled = cb.enabled();
        m_matching.callbacks[i].callbackAddr = cb.callback_addr();
        m_matching.callbacks[i].callbackArg = cb.callback_arg();
        if (cb.enabled())
            m_matching.enabledHandlersMask |= (1u << i);
    }

    if (m_matching.addr.isEmpty())
        m_matching.addr = m_socket->peerAddress().toString();

    m_matching.initialized = true;

    qInfo() << "RegisterHandlers:" << m_info.npid << "addr=" << m_matching.addr
            << "port=" << m_matching.port << "ctx=" << m_matching.ctxId
            << "handlers=0x" + QString::number(m_matching.enabledHandlersMask, 16);

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdCreateRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::CreateRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint32_t reqId = req.req_id();
    uint16_t maxSlots = static_cast<uint16_t>(req.max_slots());
    if (maxSlots < 1)
        maxSlots = 1;

    uint64_t rid = m_shared->matching.nextRoomId.fetch_add(1);

    Room room;
    room.roomId = rid;
    room.maxSlots = maxSlots;
    room.ownerNpid = m_info.npid;
    room.serverId = m_matching.serverId;
    room.worldId = req.world_id() ? static_cast<uint16_t>(req.world_id()) : m_matching.worldId;
    room.lobbyId = req.lobby_id() ? static_cast<uint16_t>(req.lobby_id()) : m_matching.lobbyId;
    room.flags = req.flags();
    room.teamId = static_cast<uint16_t>(req.team_id());
    room.groupConfigCount = static_cast<uint16_t>(req.group_config_count());
    room.allowedUserCount = static_cast<uint16_t>(req.allowed_user_count());
    room.blockedUserCount = static_cast<uint16_t>(req.blocked_user_count());
    room.internalBinAttrCount = static_cast<uint16_t>(req.internal_bin_attr_count());
    room.externalSearchIntAttrCount = static_cast<uint16_t>(req.external_search_int_attrs_size());
    room.externalSearchBinAttrCount = static_cast<uint16_t>(req.external_search_bin_attrs_size());
    room.externalBinAttrCount = static_cast<uint16_t>(req.external_bin_attrs_size());
    room.memberInternalBinAttrCount = static_cast<uint16_t>(req.member_bin_attrs_size());
    room.joinGroupLabelPresent = req.join_group_label_present();
    room.roomPasswordPresent = req.room_password_present();
    room.signalingType = static_cast<uint8_t>(req.sig_type());
    room.signalingFlag = static_cast<uint8_t>(req.sig_flag());
    room.signalingMainMember = static_cast<uint16_t>(req.sig_main_member());

    for (int i = 0; i < req.external_search_int_attrs_size(); ++i) {
        const auto& a = req.external_search_int_attrs(i);
        room.externalSearchIntAttrs.append({static_cast<uint16_t>(a.attr_id()), a.attr_value()});
    }
    for (int i = 0; i < req.external_search_bin_attrs_size(); ++i) {
        const auto& a = req.external_search_bin_attrs(i);
        room.externalSearchBinAttrs.append(
            {static_cast<uint16_t>(a.attr_id()),
             QByteArray(a.data().data(), static_cast<int>(a.data().size()))});
    }
    for (int i = 0; i < req.external_bin_attrs_size(); ++i) {
        const auto& a = req.external_bin_attrs(i);
        room.externalBinAttrs.append(
            {static_cast<uint16_t>(a.attr_id()),
             QByteArray(a.data().data(), static_cast<int>(a.data().size()))});
    }

    RoomMember* member = room.addMember(m_info.npid, m_matching.addr, m_matching.port);
    for (int i = 0; i < req.member_bin_attrs_size(); ++i) {
        const auto& a = req.member_bin_attrs(i);
        member->binAttrsInternal.append(
            {static_cast<uint16_t>(a.attr_id()),
             QByteArray(a.data().data(), static_cast<int>(a.data().size()))});
    }
    uint16_t memberId = member->memberId;

    m_matching.roomId = rid;
    m_matching.myMemberId = memberId;
    m_matching.isRoomOwner = true;
    m_matching.maxSlots = maxSlots;
    m_matching.roomFlags = room.flags;
    m_matching.serverId = room.serverId;
    m_matching.worldId = room.worldId;
    m_matching.lobbyId = room.lobbyId;

    shadnet::CreateRoomReply rep;
    rep.set_room_id(rid);
    rep.set_server_id(room.serverId);
    rep.set_world_id(room.worldId);
    rep.set_lobby_id(room.lobbyId);
    rep.set_member_id(memberId);
    rep.set_max_slots(maxSlots);
    rep.set_flags(room.flags);
    rep.set_cur_members(1);
    *rep.mutable_details() = BuildCreateJoinResponse(room, *member);
    appendProto(reply, rep);

    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray responseBlob = PbEncode(BuildCreateJoinResponse(room, *member));
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, room.serverId, room.worldId, room.lobbyId,
                                    0x0101, reqId, 0, rid, memberId, maxSlots, room.flags,
                                    true, responseBlob));
        qDebug() << "  -> RequestEvent(0x0101/CreateJoinRoom) to" << m_info.npid;
    }

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        m_shared->matching.rooms.insert(rid, std::move(room));
    }

    qInfo() << "Room" << rid << "created by" << m_info.npid << "max=" << maxSlots;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdJoinRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::JoinRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();
    uint32_t reqId = req.req_id();

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

        for (const auto& m : room.members)
            existingMembers.append({m.npid, m.memberId, m.addr, m.port});

        RoomMember* member = room.addMember(m_info.npid, m_matching.addr, m_matching.port);
        for (int i = 0; i < req.member_bin_attrs_size(); ++i) {
            const auto& a = req.member_bin_attrs(i);
            member->binAttrsInternal.append(
                {static_cast<uint16_t>(a.attr_id()),
                 QByteArray(a.data().data(), static_cast<int>(a.data().size()))});
        }
        myMemberId = member->memberId;
        maxSlots = room.maxSlots;
        flags = room.flags;

        m_matching.roomId = roomId;
        m_matching.myMemberId = myMemberId;
        m_matching.isRoomOwner = false;
        m_matching.maxSlots = maxSlots;
        m_matching.roomFlags = flags;
        m_matching.serverId = room.serverId;
        m_matching.worldId = room.worldId;
        m_matching.lobbyId = room.lobbyId;

        uint16_t curMemberNum = static_cast<uint16_t>(room.members.size());
        shadnet::JoinRoomReply rep;
        rep.set_room_id(roomId);
        rep.set_member_id(myMemberId);
        rep.set_max_slots(maxSlots);
        rep.set_flags(flags);
        rep.set_cur_members(curMemberNum);
        *rep.mutable_details() = BuildCreateJoinResponse(room, *member);
        appendProto(reply, rep);

        if (m_matching.hasHandler(HandlerType::Request))
            responseBlob = PbEncode(BuildCreateJoinResponse(room, *member));
    }

    qInfo() << "Room" << roomId << m_info.npid << "joined mid=" << myMemberId;

    // MemberJoined notification to existing members
    {
        shadnet::NotifyMemberJoined pb;
        pb.set_room_id(roomId);
        pb.set_member_id(myMemberId);
        pb.set_npid(m_info.npid.toStdString());
        pb.set_addr(m_matching.addr.toStdString());
        pb.set_port(m_matching.port);
        for (int i = 0; i < req.member_bin_attrs_size(); ++i) {
            const auto& a = req.member_bin_attrs(i);
            auto* ba = pb.add_bin_attrs();
            ba->set_attr_id(a.attr_id());
            ba->set_data(a.data());
        }
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberJoined, payload, roomId, m_info.npid);
        qDebug() << "  -> MemberJoined room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to existing members";
    }

    // Exchange SignalingHelper with existing members
    for (const auto& em : existingMembers) {
        if (m_matching.hasHandler(HandlerType::Signaling)) {
            shadnet::NotifySignalingHelper pb;
            pb.set_npid(em.npid.toStdString());
            pb.set_member_id(em.memberId);
            pb.set_addr(em.addr.toStdString());
            pb.set_port(em.port);
            QByteArray payload;
            appendProto(payload, pb);
            SendSelfNotification(NotificationType::SignalingHelper, payload);
            qDebug() << "  -> SignalingHelper to" << m_info.npid << "peer=" << em.npid
                     << "mid=" << em.memberId;
        }
        {
            shadnet::NotifySignalingHelper pb;
            pb.set_npid(m_info.npid.toStdString());
            pb.set_member_id(myMemberId);
            pb.set_addr(m_matching.addr.toStdString());
            pb.set_port(m_matching.port);
            QByteArray payload;
            appendProto(payload, pb);
            SendMatchingNotification(NotificationType::SignalingHelper, payload, em.npid);
            qDebug() << "  -> SignalingHelper to" << em.npid << "peer=" << m_info.npid
                     << "mid=" << myMemberId;
        }
    }

    // RequestEvent (0x0102) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId, m_matching.worldId,
                                    m_matching.lobbyId, 0x0102, reqId, 0, roomId, myMemberId,
                                    maxSlots, flags, false, responseBlob));
        qDebug() << "  -> RequestEvent(0x0102/JoinRoom) to" << m_info.npid;
    }

    // Send current room bin attrs to joining client (RoomDataInternalUpdated)
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
            shadnet::NotifyRoomDataInternalUpdated pb;
            pb.set_room_id(roomId);
            pb.set_flags(roomFlags);
            for (const auto& a : binSnapshot) {
                auto* ba = pb.add_bin_attrs();
                ba->set_attr_id(a.attrId);
                ba->set_data(a.data.constData(), a.data.size());
            }
            QByteArray payload;
            appendProto(payload, pb);
            SendSelfNotification(NotificationType::RoomDataInternalUpdated, payload);
            qDebug() << "  -> RoomDataInternalUpdated room=" << roomId << "to joiner" << m_info.npid
                     << "attrs=" << binSnapshot.size();
        }
    }

    // Delayed ESTABLISHED push (2 seconds)
    bool myHasSignaling = m_matching.hasHandler(HandlerType::Signaling);
    QTimer::singleShot(2000, this, [=]() {
        for (const auto& em : existingMembers) {
            shadnet::NotifySignalingEvent pb;
            pb.set_event_type(0x5102);
            pb.set_room_id(roomId);
            pb.set_member_id(myMemberId);
            pb.set_conn_id(myMemberId);
            QByteArray payload;
            appendProto(payload, pb);
            SendMatchingNotification(NotificationType::SignalingEvent, payload, em.npid);
            qDebug() << "  -> SignalingEvent(0x5102/ESTABLISHED) to" << em.npid
                     << "peer mid=" << myMemberId << "room=" << roomId;
        }
        if (myHasSignaling) {
            for (const auto& em : existingMembers) {
                shadnet::NotifySignalingEvent pb;
                pb.set_event_type(0x5102);
                pb.set_room_id(roomId);
                pb.set_member_id(em.memberId);
                pb.set_conn_id(em.memberId);
                QByteArray payload;
                appendProto(payload, pb);
                SendSelfNotification(NotificationType::SignalingEvent, payload);
                qDebug() << "  -> SignalingEvent(0x5102/ESTABLISHED) to joiner" << m_info.npid
                         << "peer mid=" << em.memberId << "room=" << roomId;
            }
        }
    });

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdLeaveRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::LeaveRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();
    uint32_t reqId = req.req_id();

    uint16_t myMemberId = 0;
    bool wasOwner = false;
    bool roomDestroyed = false;
    QVector<QPair<QString, uint16_t>> remainingMembers; // (npid, memberId)

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
            if (wasOwner) {
                room.ownerNpid = room.members.begin()->npid;
                qInfo() << "Room" << roomId << "owner ->" << room.ownerNpid;
            }
            for (const auto& m : room.members)
                remainingMembers.append({m.npid, m.memberId});
        }
    }

    ResetMatchingRoomState(roomId);
    qInfo() << "Room" << roomId << m_info.npid << "left mid=" << myMemberId;

    // Notify remaining members (MemberLeft) and tear down NpSignaling connections
    if (!roomDestroyed) {
        shadnet::NotifyMemberLeft pb;
        pb.set_room_id(roomId);
        pb.set_member_id(myMemberId);
        pb.set_npid(m_info.npid.toStdString());
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, roomId);
        qDebug() << "  -> MemberLeft room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to remaining members";

        // 0x5101 immediately to all affected parties
        for (const auto& rm : remainingMembers) {
            {
                shadnet::NotifySignalingEvent sig;
                sig.set_event_type(0x5101);
                sig.set_room_id(roomId);
                sig.set_member_id(myMemberId);
                sig.set_conn_id(myMemberId);
                QByteArray sigPayload;
                appendProto(sigPayload, sig);
                SendMatchingNotification(NotificationType::SignalingEvent, sigPayload, rm.first);
                qDebug() << "  -> 0x5101 to" << rm.first << "peer mid=" << myMemberId;
            }
            {
                shadnet::NotifySignalingEvent sig;
                sig.set_event_type(0x5101);
                sig.set_room_id(roomId);
                sig.set_member_id(rm.second);
                sig.set_conn_id(rm.second);
                QByteArray sigPayload;
                appendProto(sigPayload, sig);
                SendSelfNotification(NotificationType::SignalingEvent, sigPayload);
                qDebug() << "  -> 0x5101 to" << m_info.npid << "peer mid=" << rm.second;
            }
        }

        // NpSignaling DEAD 2 seconds later
        QString leaverNpid = m_info.npid;
        QTimer::singleShot(2000, this, [=]() {
            for (const auto& rm : remainingMembers) {
                {
                    shadnet::NotifyNpSignalingEvent dead;
                    dead.set_event(0);
                    dead.set_npid(leaverNpid.toStdString());
                    dead.set_error_code(NP_SIG_ERROR_TERMINATED_BY_PEER);
                    QByteArray deadPayload;
                    appendProto(deadPayload, dead);
                    SendMatchingNotification(NotificationType::NpSignalingEvent, deadPayload, rm.first);
                    qDebug() << "  -> NpSignaling DEAD(TERMINATED_BY_PEER) to" << rm.first
                             << "peer=" << leaverNpid;
                }
                {
                    shadnet::NotifyNpSignalingEvent dead;
                    dead.set_event(0);
                    dead.set_npid(rm.first.toStdString());
                    dead.set_error_code(NP_SIG_ERROR_TERMINATED_BY_MYSELF);
                    QByteArray deadPayload;
                    appendProto(deadPayload, dead);
                    SendMatchingNotification(NotificationType::NpSignalingEvent, deadPayload,
                                            leaverNpid);
                    qDebug() << "  -> NpSignaling DEAD(TERMINATED_BY_MYSELF) to" << leaverNpid
                             << "peer=" << rm.first;
                }
            }
        });
    }

    // RequestEvent (0x0103) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        shadnet::LeaveRoomReply leaveBlob;
        leaveBlob.set_room_id(roomId);
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId, m_matching.worldId,
                                    m_matching.lobbyId, 0x0103, reqId, 0, roomId, myMemberId,
                                    0, 0, false, PbEncode(leaveBlob)));
        qDebug() << "  -> RequestEvent(0x0103/LeaveRoom) to" << m_info.npid;
    }


    shadnet::LeaveRoomReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdKickoutRoomMember(StreamExtractor& data, QByteArray& reply) {
    shadnet::KickoutRoomMemberRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();
    uint32_t reqId = req.req_id();
    uint16_t targetMemberId = static_cast<uint16_t>(req.target_member_id());
    uint32_t blockKickFlag = req.block_kick_flag();

    QString targetNpid;
    uint16_t initiatorMemberId = 0;
    uint16_t maxSlots = 0;
    uint32_t roomFlags = 0;
    uint16_t serverId = m_matching.serverId;
    uint16_t worldId = m_matching.worldId;
    uint16_t lobbyId = m_matching.lobbyId;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();
        const RoomMember* initiator = room.findByNpid(m_info.npid);
        if (!initiator)
            return ErrorType::RoomMissing;
        if (room.ownerNpid != m_info.npid)
            return ErrorType::Unauthorized;
        if (targetMemberId == 0 || targetMemberId == initiator->memberId)
            return ErrorType::InvalidInput;
        auto targetIt = room.members.find(targetMemberId);
        if (targetIt == room.members.end())
            return ErrorType::NotFound;

        initiatorMemberId = initiator->memberId;
        targetNpid = targetIt->npid;
        maxSlots = room.maxSlots;
        roomFlags = room.flags;
        serverId = room.serverId;
        worldId = room.worldId;
        lobbyId = room.lobbyId;
    }

    qInfo() << "Room" << roomId << m_info.npid << "kicked" << targetNpid
            << "mid=" << targetMemberId << "blockKickFlag=" << blockKickFlag;

    {
        shadnet::NotifyKickedOut pb;
        pb.set_room_id(roomId);
        pb.set_status_code(MATCHING2_KICKEDOUT_STATUS_CODE);
        pb.set_guard_value(MATCHING2_KICKEDOUT_GUARD_VALUE);
        QByteArray payload;
        appendProto(payload, pb);
        SendMatchingNotification(NotificationType::KickedOut, payload, targetNpid);
        qDebug() << "  -> KickedOut room=" << roomId << "to" << targetNpid;
    }

    if (m_matching.hasHandler(HandlerType::Request)) {
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, serverId, worldId, lobbyId, 0x0104,
                                    reqId, 0, roomId, initiatorMemberId, maxSlots, roomFlags,
                                    true, {}));
        qDebug() << "  -> RequestEvent(0x0104/KickoutRoomMember) to" << m_info.npid;
    }

    shadnet::KickoutRoomMemberReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetRoomList(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(data);

    shadnet::GetRoomListReply rep;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (const auto& room : m_shared->matching.rooms) {
            *rep.add_rooms() = BuildRoomDataExternal(room);
            qInfo() << "GetRoomList: room" << room.roomId << "owner=" << room.ownerNpid
                    << "members=" << room.members.size() << "maxSlots=" << room.maxSlots
                    << "flags=" << Qt::hex << room.flags;
        }
    }
    qInfo() << "GetRoomList:" << m_info.npid << "— returning" << rep.rooms_size() << "rooms";
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRequestSignalingInfos(StreamExtractor& data, QByteArray& reply) {
    shadnet::RequestSignalingInfosRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString targetNpid = QString::fromStdString(req.target_npid());

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

    shadnet::RequestSignalingInfosReply rep;
    rep.set_target_npid(targetNpid.toStdString());
    rep.set_target_ip(targetIp.toStdString());
    rep.set_target_port(targetPort);
    rep.set_target_member_id(targetMemberId);
    appendProto(reply, rep);

    {
        shadnet::NotifySignalingHelper pb;
        pb.set_npid(m_info.npid.toStdString());
        pb.set_member_id(myMemberId);
        pb.set_addr(myIp.toStdString());
        pb.set_port(myPort);
        QByteArray payload;
        appendProto(payload, pb);
        SendMatchingNotification(NotificationType::SignalingHelper, payload, targetNpid);
        qDebug() << "  -> SignalingHelper to" << targetNpid << "peer=" << m_info.npid
                 << "mid=" << myMemberId;
    }

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSignalingEstablished(StreamExtractor& data) {
    shadnet::SignalingEstablishedRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString targetNpid = QString::fromStdString(req.target_npid());
    uint32_t connId = req.conn_id();

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
    shadnet::ActivationConfirmRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString meId = QString::fromStdString(req.me_id());
    QString initiatorIpStr = QString::fromStdString(req.initiator_ip());
    uint32_t ctxTag = req.ctx_tag();

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

    shadnet::NotifyNpSignalingEvent pb;
    pb.set_event(1);
    pb.set_npid(meId.toStdString());
    QByteArray payload;
    appendProto(payload, pb);
    SendMatchingNotification(NotificationType::NpSignalingEvent, payload, initiatorNpid);

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdCancelActivationIntent(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(reply);
    shadnet::CancelActivationIntentRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const QString meNpid = QString::fromStdString(req.me_npid());
    const QString peerNpid = QString::fromStdString(req.peer_npid());

    int removed = 0;
    {
        QWriteLocker lk(&m_shared->matching.activationLock);
        auto& intents = m_shared->matching.activationIntents;
        for (auto it = intents.begin(); it != intents.end(); ) {
            if (it->first == meNpid && it->second == peerNpid) {
                it = intents.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }

    qInfo() << "CancelActivationIntent: me=" << meNpid << "peer=" << peerNpid
            << "removed=" << removed;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSetRoomDataInternal(StreamExtractor& data, QByteArray& reply) {
    shadnet::SetRoomDataInternalRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint32_t reqId = req.req_id();
    uint64_t roomId = req.room_id();
    uint32_t flagFilter = req.flag_filter();
    uint32_t flagAttr = req.flag_attr();
    bool hasPasswdMask = req.has_passwd_mask();
    uint64_t passwdSlotMask = req.passwd_slot_mask();

    uint32_t newFlags = 0;
    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        room.flags = (room.flags & ~flagFilter) | (flagAttr & flagFilter);
        newFlags = room.flags;

        if (hasPasswdMask)
            room.passwdSlotMask = passwdSlotMask;

        for (int i = 0; i < req.bin_attrs_size(); ++i) {
            const auto& a = req.bin_attrs(i);
            uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            QByteArray attrData(a.data().data(), static_cast<int>(a.data().size()));
            bool found = false;
            for (auto& existing : room.binAttrsInternal) {
                if (existing.attrId == attrId) {
                    existing.data = attrData;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.binAttrsInternal.append({attrId, attrData});
        }

        qInfo() << "SetRoomDataInternal: room=" << roomId << "flags=" << Qt::hex << newFlags
                << "binAttrs=" << req.bin_attrs_size();
    }

    // RequestEvent(0x0109) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        shadnet::SetRoomDataInternalReply blob;
        blob.set_room_id(roomId);
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId, m_matching.worldId,
                                    m_matching.lobbyId, 0x0109, reqId, 0, roomId,
                                    m_matching.myMemberId, m_matching.maxSlots, newFlags,
                                    m_matching.isRoomOwner, PbEncode(blob)));
        qDebug() << "  -> RequestEvent(0x0109/SetRoomDataInternal) to" << m_info.npid;
    }

    // Broadcast RoomDataInternalUpdated to all other members
    {
        shadnet::NotifyRoomDataInternalUpdated pb;
        pb.set_room_id(roomId);
        pb.set_flags(newFlags);
        for (int i = 0; i < req.bin_attrs_size(); ++i) {
            const auto& a = req.bin_attrs(i);
            auto* ba = pb.add_bin_attrs();
            ba->set_attr_id(a.attr_id());
            ba->set_data(a.data());
        }
        QByteArray notifPayload;
        appendProto(notifPayload, pb);
        NotifyRoomMembers(NotificationType::RoomDataInternalUpdated, notifPayload, roomId,
                          m_info.npid);
        qDebug() << "  -> RoomDataInternalUpdated room=" << roomId << "broadcast (excl. sender)"
                 << "attrs=" << req.bin_attrs_size();
    }

    shadnet::SetRoomDataInternalReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSetRoomDataExternal(StreamExtractor& data, QByteArray& reply) {
    shadnet::SetRoomDataExternalRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint32_t reqId = req.req_id();
    uint64_t roomId = req.room_id();

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find(roomId);
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        for (int i = 0; i < req.search_int_attrs_size(); ++i) {
            const auto& a = req.search_int_attrs(i);
            uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            bool found = false;
            for (auto& existing : room.externalSearchIntAttrs) {
                if (existing.attrId == attrId) {
                    existing.attrValue = a.attr_value();
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalSearchIntAttrs.append({attrId, a.attr_value()});
        }
        for (int i = 0; i < req.search_bin_attrs_size(); ++i) {
            const auto& a = req.search_bin_attrs(i);
            uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            QByteArray attrData(a.data().data(), static_cast<int>(a.data().size()));
            bool found = false;
            for (auto& existing : room.externalSearchBinAttrs) {
                if (existing.attrId == attrId) {
                    existing.data = attrData;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalSearchBinAttrs.append({attrId, attrData});
        }
        for (int i = 0; i < req.ext_bin_attrs_size(); ++i) {
            const auto& a = req.ext_bin_attrs(i);
            uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            QByteArray attrData(a.data().data(), static_cast<int>(a.data().size()));
            bool found = false;
            for (auto& existing : room.externalBinAttrs) {
                if (existing.attrId == attrId) {
                    existing.data = attrData;
                    found = true;
                    break;
                }
            }
            if (!found)
                room.externalBinAttrs.append({attrId, attrData});
        }

        qInfo() << "SetRoomDataExternal: room=" << roomId
                << "searchIntAttrs=" << req.search_int_attrs_size()
                << "searchBinAttrs=" << req.search_bin_attrs_size()
                << "extBinAttrs=" << req.ext_bin_attrs_size();
    }

    // RequestEvent(0x0004) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        shadnet::SetRoomDataExternalReply blob;
        blob.set_room_id(roomId);
        SendSelfNotification(NotificationType::RequestEvent,
            MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId, m_matching.worldId,
                                    m_matching.lobbyId, 0x0004, reqId, 0, roomId,
                                    m_matching.myMemberId, m_matching.maxSlots, m_matching.roomFlags,
                                    m_matching.isRoomOwner, PbEncode(blob)));
        qDebug() << "  -> RequestEvent(0x0004/SetRoomDataExternal) to" << m_info.npid;
    }

    shadnet::SetRoomDataExternalReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

// ── DoLeaveRoom (reusable for both CmdLeaveRoom and disconnect cleanup) ────────

void ClientSession::DoLeaveRoom(uint64_t roomId) {
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
        }
    }

    ResetMatchingRoomState(roomId);

    if (!roomDestroyed) {
        shadnet::NotifyMemberLeft pb;
        pb.set_room_id(roomId);
        pb.set_member_id(myMemberId);
        pb.set_npid(m_info.npid.toStdString());
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, roomId);
        qDebug() << "  -> MemberLeft room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to remaining members (disconnect)";
    }
}

// ── Disconnect cleanup ────────────────────────────────────────────────────────

void ClientSession::CleanupMatchingOnDisconnect() {
    if (!m_matching.initialized)
        return;

    if (m_matching.roomId != 0)
        DoLeaveRoom(m_matching.roomId);

    ClearPeerMatchingArtifacts(m_shared, m_info.npid);

    qInfo() << "Matching cleanup for" << m_info.npid;
}
