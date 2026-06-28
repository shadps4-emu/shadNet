// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QtEndian>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

namespace {

constexpr int32_t MATCHING2_KICKEDOUT_STATUS_CODE = static_cast<int32_t>(0xFF000019u);
constexpr uint32_t MATCHING2_KICKEDOUT_GUARD_VALUE = 4;

uint64_t MatchingTimestampUsec() {
    return static_cast<uint64_t>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) * 1000ull;
}

bool MatchIntFilter(uint32_t op, uint32_t roomValue, uint32_t filterValue) {
    switch (op) {
    case 1:
        return roomValue == filterValue;
    case 2:
        return roomValue != filterValue;
    case 3:
        return roomValue < filterValue;
    case 4:
        return roomValue <= filterValue;
    case 5:
        return roomValue > filterValue;
    case 6:
        return roomValue >= filterValue;
    default:
        return false;
    }
}

bool RoomMatchesFilters(const Room& room, const shadnet::GetRoomListRequest& req) {
    if (room.flagAttr & Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_HIDDEN)
        return false;
    if ((room.flagAttr & req.flag_filter()) != (req.flag_attr() & req.flag_filter()))
        return false;

    for (int i = 0; i < req.int_filters_size(); ++i) {
        const auto& f = req.int_filters(i);
        const uint16_t id = static_cast<uint16_t>(f.attr_id());
        bool found = false;
        for (const auto& slot : room.searchIntAttr) {
            if (slot.set && slot.attrId == id) {
                if (!MatchIntFilter(f.op(), slot.value, f.attr_value()))
                    return false;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }

    for (int i = 0; i < req.bin_filters_size(); ++i) {
        const auto& f = req.bin_filters(i);
        if (!room.searchBinAttr.set ||
            room.searchBinAttr.attrId != static_cast<uint16_t>(f.attr_id()))
            return false;
        const QByteArray fdata(f.data().data(), static_cast<int>(f.data().size()));
        const bool eq = room.searchBinAttr.data == fdata;
        if (f.op() == 1 && !eq)
            return false;
        if (f.op() == 2 && eq)
            return false;
    }
    return true;
}

void ResetLocalMatchingRoomState(MatchingSessionState& matching) {
    matching.roomId = 0;
    matching.myMemberId = 0;
    matching.isRoomOwner = false;
    matching.maxSlots = 0;
    matching.roomFlags = 0;
}

} // namespace

// ── Proto builder helpers ─────────────────────────────────────────────────────

static void FillRoomGroup(shadnet::MatchingRoomGroup* grp, const RoomGroup& g) {
    grp->set_group_id(g.groupId);
    grp->set_has_passwd(g.withPassword);
    grp->set_has_label(g.label.has_value());
    if (g.label.has_value())
        grp->set_label(g.label->constData(), g.label->size());
    grp->set_slot_count(g.slotNum);
    grp->set_num_members(g.numMembers);
}

static shadnet::MatchingRoomDataInternal BuildRoomDataInternal(const Room& room) {
    shadnet::MatchingRoomDataInternal pb;
    pb.set_public_slots(room.publicSlots);
    pb.set_private_slots(room.privateSlots);
    pb.set_open_public_slots(room.openPublicSlots);
    pb.set_open_private_slots(room.openPrivateSlots);
    pb.set_max_slot(room.maxSlot);
    pb.set_server_id(room.serverId);
    pb.set_world_id(room.worldId);
    pb.set_lobby_id(room.lobbyId);
    pb.set_room_id(room.roomId);
    pb.set_passwd_slot_mask(room.passwdSlotMask);
    pb.set_joined_slot_mask(room.joinedSlotMask());
    for (const auto& g : room.groups)
        FillRoomGroup(pb.add_groups(), g);
    pb.set_flags(room.flagAttr);
    for (const auto& slot : room.internalBinAttr) {
        if (!slot.set)
            continue;
        auto* a = pb.add_bin_attrs_internal();
        a->set_attr_id(slot.attrId);
        a->set_data(slot.data.constData(), slot.data.size());
    }
    return pb;
}

static shadnet::MatchingRoomMemberData BuildRoomMemberData(const RoomMember& member) {
    shadnet::MatchingRoomMemberData pb;
    pb.set_npid(member.npid.toStdString());
    pb.set_member_id(member.memberId);
    pb.set_team_id(member.teamId);
    pb.set_is_owner((member.flagAttr & Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER) !=
                    0);
    if (member.memberBinAttr.set) {
        auto* a = pb.add_bin_attrs_internal();
        a->set_attr_id(member.memberBinAttr.attrId);
        a->set_data(member.memberBinAttr.data.constData(), member.memberBinAttr.data.size());
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
        *pb.add_members() = BuildRoomMemberData(room.members[mid]);
    pb.set_me_member_id(me.memberId);
    pb.set_owner_member_id(room.ownerMemberId);
    return pb;
}

static shadnet::MatchingRoomDataExternal BuildRoomDataExternal(const Room& room) {
    shadnet::MatchingRoomDataExternal pb;
    uint16_t memberCount = static_cast<uint16_t>(room.members.size());
    pb.set_max_slot(room.maxSlot);
    pb.set_cur_members(memberCount);
    pb.set_flags(room.flagAttr);
    pb.set_server_id(room.serverId);
    pb.set_world_id(room.worldId);
    pb.set_lobby_id(room.lobbyId);
    pb.set_room_id(room.roomId);
    pb.set_passwd_slot_mask(room.passwdSlotMask);
    pb.set_joined_slot_mask(room.joinedSlotMask());
    pb.set_public_slots(room.publicSlots);
    pb.set_private_slots(room.privateSlots);
    pb.set_open_public_slots(room.openPublicSlots);
    pb.set_open_private_slots(room.openPrivateSlots);
    for (const auto& g : room.groups)
        FillRoomGroup(pb.add_groups(), g);
    for (const auto& slot : room.searchIntAttr) {
        if (!slot.set)
            continue;
        auto* ia = pb.add_external_search_int_attrs();
        ia->set_attr_id(slot.attrId);
        ia->set_attr_value(slot.value);
    }
    if (room.searchBinAttr.set) {
        auto* ba = pb.add_external_search_bin_attrs();
        ba->set_attr_id(room.searchBinAttr.attrId);
        ba->set_data(room.searchBinAttr.data.constData(), room.searchBinAttr.data.size());
    }
    for (const auto& slot : room.externalBinAttr) {
        if (!slot.set)
            continue;
        auto* ba = pb.add_external_bin_attrs();
        ba->set_attr_id(slot.attrId);
        ba->set_data(slot.data.constData(), slot.data.size());
    }
    const RoomMember* owner = const_cast<Room&>(room).findById(room.ownerMemberId);
    pb.set_owner_npid(owner ? owner->npid.toStdString() : std::string());
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
                                      const QString& comId, uint64_t roomId,
                                      const QString& excludeNpid) {
    QVector<std::function<void(QByteArray)>> senders;
    {
        QReadLocker roomLk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({comId, roomId});
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
    m_matching.comId = QString::fromStdString(req.com_id());

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
            << "comId=" << m_matching.comId
            << "handlers=0x" + QString::number(m_matching.enabledHandlersMask, 16);

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdCreateRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::CreateRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint32_t reqId = req.req_id();
    uint16_t maxSlot = static_cast<uint16_t>(req.max_slots());
    if (maxSlot < 1)
        maxSlot = 1;

    uint64_t rid = m_shared->matching.nextRoomId.fetch_add(1);

    Room room;
    room.roomId = rid;
    room.maxSlot = maxSlot;
    room.serverId = m_matching.serverId;
    room.worldId = req.world_id() ? req.world_id() : m_matching.worldId;
    room.lobbyId = req.lobby_id() ? req.lobby_id() : m_matching.lobbyId;
    room.flagAttr = req.flags() & ~Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;
    room.signalingType = static_cast<uint8_t>(req.sig_type());
    room.signalingFlag = static_cast<uint8_t>(req.sig_flag());
    room.signalingHubMemberId = static_cast<uint16_t>(req.sig_main_member());

    if (req.room_password_present())
        room.roomPassword = QByteArray(Matching2::ORBIS_NP_MATCHING2_SESSION_PASSWORD_SIZE, '\0');

    for (uint32_t g = 0; g < req.group_config_count(); ++g) {
        RoomGroup grp;
        grp.groupId = static_cast<uint8_t>(g + 1);
        grp.withPassword = req.room_password_present();
        grp.fixedLabel = req.join_group_label_present();
        room.groups.append(grp);
    }

    for (int i = 0; i < req.external_search_int_attrs_size() && i < 8; ++i) {
        const auto& a = req.external_search_int_attrs(i);
        room.searchIntAttr[i].set = true;
        room.searchIntAttr[i].attrId = static_cast<uint16_t>(a.attr_id());
        room.searchIntAttr[i].value = a.attr_value();
    }
    if (req.external_search_bin_attrs_size() > 0) {
        const auto& a = req.external_search_bin_attrs(0);
        room.searchBinAttr.set = true;
        room.searchBinAttr.attrId = static_cast<uint16_t>(a.attr_id());
        room.searchBinAttr.data = QByteArray(a.data().data(), static_cast<int>(a.data().size()));
    }
    for (int i = 0; i < req.external_bin_attrs_size() && i < 2; ++i) {
        const auto& a = req.external_bin_attrs(i);
        room.externalBinAttr[i].set = true;
        room.externalBinAttr[i].attrId = static_cast<uint16_t>(a.attr_id());
        room.externalBinAttr[i].data =
            QByteArray(a.data().data(), static_cast<int>(a.data().size()));
    }

    RoomMember owner;
    owner.userId = m_info.userId;
    owner.npid = m_info.npid;
    owner.avatarUrl = m_info.avatarUrl;
    owner.addr = m_matching.addr;
    owner.port = m_matching.port;
    owner.joinDate = MatchingTimestampUsec();
    owner.teamId = static_cast<uint8_t>(req.team_id());
    owner.flagAttr = Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
    if (req.member_bin_attrs_size() > 0) {
        const auto& a = req.member_bin_attrs(0);
        owner.memberBinAttr.set = true;
        owner.memberBinAttr.attrId = static_cast<uint16_t>(a.attr_id());
        owner.memberBinAttr.data = QByteArray(a.data().data(), static_cast<int>(a.data().size()));
        owner.memberBinAttr.updateDate = owner.joinDate;
    }

    RoomMember* member = room.addMember(owner);
    uint16_t memberId = member->memberId;
    room.ownerMemberId = memberId;

    room.publicSlots = maxSlot;
    room.openPublicSlots = static_cast<uint16_t>(maxSlot - 1);

    m_matching.roomId = rid;
    m_matching.myMemberId = memberId;
    m_matching.isRoomOwner = true;
    m_matching.maxSlots = maxSlot;
    m_matching.roomFlags = room.flagAttr;
    m_matching.serverId = room.serverId;
    m_matching.worldId = room.worldId;
    m_matching.lobbyId = room.lobbyId;

    shadnet::CreateRoomReply rep;
    rep.set_room_id(rid);
    rep.set_server_id(room.serverId);
    rep.set_world_id(room.worldId);
    rep.set_lobby_id(room.lobbyId);
    rep.set_member_id(memberId);
    rep.set_max_slots(maxSlot);
    rep.set_flags(room.flagAttr);
    rep.set_cur_members(1);
    *rep.mutable_details() = BuildCreateJoinResponse(room, *member);
    appendProto(reply, rep);

    if (m_matching.hasHandler(HandlerType::Request)) {
        QByteArray responseBlob = PbEncode(BuildCreateJoinResponse(room, *member));
        SendSelfNotification(NotificationType::RequestEvent,
                             MakeRequestEventPayload(m_matching.ctxId, room.serverId, room.worldId,
                                                     room.lobbyId, 0x0101, reqId, 0, rid, memberId,
                                                     maxSlot, room.flagAttr, true, responseBlob));
        qDebug() << "  -> RequestEvent(0x0101/CreateJoinRoom) to" << m_info.npid;
    }

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        const uint32_t worldId = room.worldId;
        const uint64_t lobbyId = room.lobbyId;
        m_shared->matching.rooms.insert({m_matching.comId, rid}, std::move(room));
        if (worldId != 0)
            m_shared->matching.worldRooms[{m_matching.comId, worldId}].append(rid);
        else if (lobbyId != 0)
            m_shared->matching.lobbyRooms[{m_matching.comId, lobbyId}].append(rid);
    }

    qInfo() << "Room" << rid << "created by" << m_info.npid << "max=" << maxSlot;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdJoinRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::JoinRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();
    uint32_t reqId = req.req_id();

    uint16_t myMemberId = 0;
    uint16_t maxSlot = 0;
    uint32_t flags = 0;
    QByteArray responseBlob;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();
        if (room.isFull())
            return ErrorType::RoomFull;
        if (room.findByNpid(m_info.npid))
            return ErrorType::RoomAlreadyJoined;

        RoomMember joiner;
        joiner.userId = m_info.userId;
        joiner.npid = m_info.npid;
        joiner.avatarUrl = m_info.avatarUrl;
        joiner.addr = m_matching.addr;
        joiner.port = m_matching.port;
        joiner.joinDate = MatchingTimestampUsec();
        joiner.teamId = static_cast<uint8_t>(req.team_id());
        if (req.member_bin_attrs_size() > 0) {
            const auto& a = req.member_bin_attrs(0);
            joiner.memberBinAttr.set = true;
            joiner.memberBinAttr.attrId = static_cast<uint16_t>(a.attr_id());
            joiner.memberBinAttr.data =
                QByteArray(a.data().data(), static_cast<int>(a.data().size()));
            joiner.memberBinAttr.updateDate = joiner.joinDate;
        }

        RoomMember* member = room.addMember(joiner);
        myMemberId = member->memberId;
        maxSlot = room.maxSlot;
        flags = room.flagAttr;

        const uint16_t curMemberNum = static_cast<uint16_t>(room.members.size());
        room.openPublicSlots =
            static_cast<uint16_t>(room.maxSlot > curMemberNum ? room.maxSlot - curMemberNum : 0);
        if (curMemberNum >= room.maxSlot)
            room.flagAttr |= Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;

        m_matching.roomId = roomId;
        m_matching.myMemberId = myMemberId;
        m_matching.isRoomOwner = false;
        m_matching.maxSlots = maxSlot;
        m_matching.roomFlags = room.flagAttr;
        m_matching.serverId = room.serverId;
        m_matching.worldId = room.worldId;
        m_matching.lobbyId = room.lobbyId;

        shadnet::JoinRoomReply rep;
        rep.set_room_id(roomId);
        rep.set_member_id(myMemberId);
        rep.set_max_slots(maxSlot);
        rep.set_flags(room.flagAttr);
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
        NotifyRoomMembers(NotificationType::MemberJoined, payload, m_matching.comId, roomId,
                          m_info.npid);
        qDebug() << "  -> MemberJoined room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to existing members";
    }

    // RequestEvent (0x0102) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        SendSelfNotification(NotificationType::RequestEvent,
                             MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId,
                                                     m_matching.worldId, m_matching.lobbyId, 0x0102,
                                                     reqId, 0, roomId, myMemberId, maxSlot, flags,
                                                     false, responseBlob));
        qDebug() << "  -> RequestEvent(0x0102/JoinRoom) to" << m_info.npid;
    }

    // Send current room bin attrs to joining client (RoomDataInternalUpdated)
    if (m_matching.hasHandler(HandlerType::RoomEvent)) {
        QVector<InternalBinAttrSlot> binSnapshot;
        uint32_t roomFlags = 0;
        {
            QReadLocker lk(&m_shared->matching.roomsLock);
            auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
            if (roomIt != m_shared->matching.rooms.end()) {
                for (const auto& slot : roomIt->internalBinAttr)
                    if (slot.set)
                        binSnapshot.append(slot);
                roomFlags = roomIt->flagAttr;
            }
        }
        if (!binSnapshot.isEmpty()) {
            shadnet::NotifyRoomDataInternalUpdated pb;
            pb.set_room_id(roomId);
            pb.set_flags(roomFlags);
            pb.set_ctx_id(m_matching.ctxId);
            for (const auto& slot : binSnapshot) {
                auto* ba = pb.add_bin_attrs();
                ba->set_attr_id(slot.attrId);
                ba->set_data(slot.data.constData(), slot.data.size());
            }
            QByteArray payload;
            appendProto(payload, pb);
            SendSelfNotification(NotificationType::RoomDataInternalUpdated, payload);
            qDebug() << "  -> RoomDataInternalUpdated room=" << roomId << "to joiner" << m_info.npid
                     << "attrs=" << binSnapshot.size();
        }
    }

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

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        const RoomMember* me = room.findByNpid(m_info.npid);
        if (!me)
            return ErrorType::RoomMissing;
        myMemberId = me->memberId;
        wasOwner = (myMemberId == room.ownerMemberId);

        const uint32_t worldId = room.worldId;
        const uint64_t lobbyId = room.lobbyId;

        room.removeMember(myMemberId);
        room.ownerSuccession.removeAll(myMemberId);

        if (room.isEmpty()) {
            m_shared->matching.rooms.erase(roomIt);
            if (worldId != 0)
                m_shared->matching.worldRooms[{m_matching.comId, worldId}].removeAll(roomId);
            else if (lobbyId != 0)
                m_shared->matching.lobbyRooms[{m_matching.comId, lobbyId}].removeAll(roomId);
            roomDestroyed = true;
            qInfo() << "Room" << roomId << "destroyed";
        } else {
            const uint16_t curMemberNum = static_cast<uint16_t>(room.members.size());
            room.openPublicSlots = static_cast<uint16_t>(
                room.maxSlot > curMemberNum ? room.maxSlot - curMemberNum : 0);
            room.flagAttr &= ~Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;

            if (wasOwner) {
                uint16_t newOwner = 0;
                while (!room.ownerSuccession.isEmpty()) {
                    const uint16_t cand = room.ownerSuccession.takeFirst();
                    if (room.members.contains(cand)) {
                        newOwner = cand;
                        break;
                    }
                }
                if (newOwner == 0)
                    newOwner = room.members.begin()->memberId;
                room.ownerMemberId = newOwner;
                if (RoomMember* om = room.findById(newOwner))
                    om->flagAttr |= Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
                qInfo() << "Room" << roomId << "owner -> mid" << newOwner;
            }
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
        pb.set_ctx_id(m_matching.ctxId);
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, m_matching.comId, roomId);
        qDebug() << "  -> MemberLeft room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to remaining members";
    }

    // RequestEvent (0x0103) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        shadnet::LeaveRoomReply leaveBlob;
        leaveBlob.set_room_id(roomId);
        SendSelfNotification(NotificationType::RequestEvent,
                             MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId,
                                                     m_matching.worldId, m_matching.lobbyId, 0x0103,
                                                     reqId, 0, roomId, myMemberId, 0, 0, false,
                                                     PbEncode(leaveBlob)));
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
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();
        const RoomMember* initiator = room.findByNpid(m_info.npid);
        if (!initiator)
            return ErrorType::RoomMissing;
        if (room.ownerMemberId != initiator->memberId)
            return ErrorType::Unauthorized;
        if (targetMemberId == 0 || targetMemberId == initiator->memberId)
            return ErrorType::InvalidInput;
        auto targetIt = room.members.find(targetMemberId);
        if (targetIt == room.members.end())
            return ErrorType::NotFound;

        initiatorMemberId = initiator->memberId;
        targetNpid = targetIt->npid;
        maxSlots = room.maxSlot;
        roomFlags = room.flagAttr;
        serverId = room.serverId;
        worldId = room.worldId;
        lobbyId = room.lobbyId;

        if (blockKickFlag && !room.blockedUsers.contains(targetNpid))
            room.blockedUsers.append(targetNpid);

        room.removeMember(targetMemberId);
        room.ownerSuccession.removeAll(targetMemberId);
        const uint16_t curMemberNum = static_cast<uint16_t>(room.members.size());
        room.openPublicSlots =
            static_cast<uint16_t>(room.maxSlot > curMemberNum ? room.maxSlot - curMemberNum : 0);
        room.flagAttr &= ~Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;
    }

    qInfo() << "Room" << roomId << m_info.npid << "kicked" << targetNpid << "mid=" << targetMemberId
            << "blockKickFlag=" << blockKickFlag;

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

    {
        shadnet::NotifyMemberLeft pb;
        pb.set_room_id(roomId);
        pb.set_member_id(targetMemberId);
        pb.set_npid(targetNpid.toStdString());
        pb.set_ctx_id(m_matching.ctxId);
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, m_matching.comId, roomId,
                          targetNpid);
        qDebug() << "  -> MemberLeft (kicked) room=" << roomId << "mid=" << targetMemberId;
    }

    if (m_matching.hasHandler(HandlerType::Request)) {
        SendSelfNotification(NotificationType::RequestEvent,
                             MakeRequestEventPayload(m_matching.ctxId, serverId, worldId, lobbyId,
                                                     0x0104, reqId, 0, roomId, initiatorMemberId,
                                                     maxSlots, roomFlags, true, {}));
        qDebug() << "  -> RequestEvent(0x0104/KickoutRoomMember) to" << m_info.npid;
    }

    shadnet::KickoutRoomMemberReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetRoomList(StreamExtractor& data, QByteArray& reply) {
    shadnet::GetRoomListRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const uint32_t worldId = req.world_id();
    const uint64_t lobbyId = req.lobby_id();

    uint32_t rangeStart = req.range_filter_start();
    if (rangeStart < 1)
        rangeStart = 1;
    uint32_t rangeMax = req.range_filter_max();
    if (rangeMax == 0 || rangeMax > 20)
        rangeMax = 20;

    shadnet::GetRoomListReply rep;
    uint32_t total = 0;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);

        QVector<uint64_t> candidates;
        if (worldId != 0)
            candidates = m_shared->matching.worldRooms.value({m_matching.comId, worldId});
        else if (lobbyId != 0)
            candidates = m_shared->matching.lobbyRooms.value({m_matching.comId, lobbyId});

        QVector<const Room*> matches;
        for (uint64_t rid : candidates) {
            auto roomIt = m_shared->matching.rooms.find({m_matching.comId, rid});
            if (roomIt == m_shared->matching.rooms.end())
                continue;
            if (!RoomMatchesFilters(roomIt.value(), req))
                continue;
            matches.append(&roomIt.value());
        }

        total = static_cast<uint32_t>(matches.size());
        uint32_t emitted = 0;
        for (uint32_t i = rangeStart - 1;
             i < static_cast<uint32_t>(matches.size()) && emitted < rangeMax; ++i, ++emitted) {
            *rep.add_rooms() = BuildRoomDataExternal(*matches[i]);
        }
    }

    rep.set_range_start(rangeStart);
    rep.set_range_total(total);
    rep.set_range_result(static_cast<uint32_t>(rep.rooms_size()));
    appendProto(reply, rep);

    qInfo() << "GetRoomList:" << m_info.npid << "world=" << worldId << "lobby=" << lobbyId
            << "total=" << total << "returned=" << rep.rooms_size();
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
        for (auto it = m_shared->matching.rooms.constBegin();
             it != m_shared->matching.rooms.constEnd(); ++it) {
            if (it.key().first != m_matching.comId)
                continue;
            const RoomMember* tm = it.value().findByNpid(targetNpid);
            if (tm) {
                targetIp = tm->addr;
                targetPort = tm->port;
                break;
            }
        }
    }
    if (targetIp.isEmpty())
        return ErrorType::NotFound;

    uint16_t targetMemberId = 0;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        for (auto it = m_shared->matching.rooms.constBegin();
             it != m_shared->matching.rooms.constEnd(); ++it) {
            if (it.key().first != m_matching.comId)
                continue;
            const RoomMember* tm = it.value().findByNpid(targetNpid);
            if (tm) {
                targetMemberId = tm->memberId;
                break;
            }
        }
    }

    qInfo() << "RequestSignalingInfos:" << m_info.npid << "->" << targetNpid
            << "(mid=" << targetMemberId << ") target=" << targetIp << ":" << targetPort;

    shadnet::RequestSignalingInfosReply rep;
    rep.set_target_npid(targetNpid.toStdString());
    rep.set_target_ip(targetIp.toStdString());
    rep.set_target_port(targetPort);
    rep.set_target_member_id(targetMemberId);
    appendProto(reply, rep);

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
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        const uint32_t protectedMask = Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;
        const uint32_t effFilter = flagFilter & ~protectedMask;
        room.flagAttr = (room.flagAttr & ~effFilter) | (flagAttr & effFilter);
        newFlags = room.flagAttr;

        if (hasPasswdMask)
            room.passwdSlotMask = passwdSlotMask;

        const RoomMember* setter = room.findByNpid(m_info.npid);
        const uint16_t setterId = setter ? setter->memberId : 0;
        const uint64_t now = MatchingTimestampUsec();
        for (int i = 0; i < req.bin_attrs_size(); ++i) {
            const auto& a = req.bin_attrs(i);
            const uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            int idx = -1;
            if (attrId == Matching2::ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_INTERNAL_1_ID)
                idx = 0;
            else if (attrId == Matching2::ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_INTERNAL_2_ID)
                idx = 1;
            if (idx < 0)
                continue;
            auto& slot = room.internalBinAttr[idx];
            slot.set = true;
            slot.attrId = attrId;
            slot.data = QByteArray(a.data().data(), static_cast<int>(a.data().size()));
            slot.updateDate = now;
            slot.updateMemberId = setterId;
        }

        qInfo() << "SetRoomDataInternal: room=" << roomId << "flags=" << Qt::hex << newFlags
                << "binAttrs=" << req.bin_attrs_size();
    }

    // RequestEvent(0x0109) to self
    if (m_matching.hasHandler(HandlerType::Request)) {
        shadnet::SetRoomDataInternalReply blob;
        blob.set_room_id(roomId);
        SendSelfNotification(NotificationType::RequestEvent,
                             MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId,
                                                     m_matching.worldId, m_matching.lobbyId, 0x0109,
                                                     reqId, 0, roomId, m_matching.myMemberId,
                                                     m_matching.maxSlots, newFlags,
                                                     m_matching.isRoomOwner, PbEncode(blob)));
        qDebug() << "  -> RequestEvent(0x0109/SetRoomDataInternal) to" << m_info.npid;
    }

    // Broadcast RoomDataInternalUpdated to all other members
    {
        shadnet::NotifyRoomDataInternalUpdated pb;
        pb.set_room_id(roomId);
        pb.set_flags(newFlags);
        pb.set_ctx_id(m_matching.ctxId);
        for (int i = 0; i < req.bin_attrs_size(); ++i) {
            const auto& a = req.bin_attrs(i);
            auto* ba = pb.add_bin_attrs();
            ba->set_attr_id(a.attr_id());
            ba->set_data(a.data());
        }
        QByteArray notifPayload;
        appendProto(notifPayload, pb);
        NotifyRoomMembers(NotificationType::RoomDataInternalUpdated, notifPayload, m_matching.comId,
                          roomId, m_info.npid);
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
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();

        for (int i = 0; i < req.search_int_attrs_size(); ++i) {
            const auto& a = req.search_int_attrs(i);
            const uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            if (attrId < Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_1_ID ||
                attrId > Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_8_ID)
                continue;
            const int idx =
                attrId - Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_1_ID;
            room.searchIntAttr[idx].set = true;
            room.searchIntAttr[idx].attrId = attrId;
            room.searchIntAttr[idx].value = a.attr_value();
        }
        for (int i = 0; i < req.search_bin_attrs_size(); ++i) {
            const auto& a = req.search_bin_attrs(i);
            const uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            if (attrId != Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_BIN_ATTR_EXTERNAL_1_ID)
                continue;
            room.searchBinAttr.set = true;
            room.searchBinAttr.attrId = attrId;
            room.searchBinAttr.data =
                QByteArray(a.data().data(), static_cast<int>(a.data().size()));
        }
        for (int i = 0; i < req.ext_bin_attrs_size(); ++i) {
            const auto& a = req.ext_bin_attrs(i);
            const uint16_t attrId = static_cast<uint16_t>(a.attr_id());
            int idx = -1;
            if (attrId == Matching2::ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_EXTERNAL_1_ID)
                idx = 0;
            else if (attrId == Matching2::ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_EXTERNAL_2_ID)
                idx = 1;
            if (idx < 0)
                continue;
            room.externalBinAttr[idx].set = true;
            room.externalBinAttr[idx].attrId = attrId;
            room.externalBinAttr[idx].data =
                QByteArray(a.data().data(), static_cast<int>(a.data().size()));
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
                             MakeRequestEventPayload(m_matching.ctxId, m_matching.serverId,
                                                     m_matching.worldId, m_matching.lobbyId, 0x0004,
                                                     reqId, 0, roomId, m_matching.myMemberId,
                                                     m_matching.maxSlots, m_matching.roomFlags,
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
        auto roomIt = m_shared->matching.rooms.find({m_matching.comId, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return;
        Room& room = roomIt.value();

        const RoomMember* me = room.findByNpid(m_info.npid);
        if (!me)
            return;
        myMemberId = me->memberId;
        const bool wasOwner = (myMemberId == room.ownerMemberId);

        const uint32_t worldId = room.worldId;
        const uint64_t lobbyId = room.lobbyId;

        room.removeMember(myMemberId);
        room.ownerSuccession.removeAll(myMemberId);

        if (room.isEmpty()) {
            m_shared->matching.rooms.erase(roomIt);
            if (worldId != 0)
                m_shared->matching.worldRooms[{m_matching.comId, worldId}].removeAll(roomId);
            else if (lobbyId != 0)
                m_shared->matching.lobbyRooms[{m_matching.comId, lobbyId}].removeAll(roomId);
            roomDestroyed = true;
            qInfo() << "Room" << roomId << "destroyed (disconnect)";
        } else {
            const uint16_t curMemberNum = static_cast<uint16_t>(room.members.size());
            room.openPublicSlots = static_cast<uint16_t>(
                room.maxSlot > curMemberNum ? room.maxSlot - curMemberNum : 0);
            room.flagAttr &= ~Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_FULL;
            if (wasOwner) {
                uint16_t newOwner = 0;
                while (!room.ownerSuccession.isEmpty()) {
                    const uint16_t cand = room.ownerSuccession.takeFirst();
                    if (room.members.contains(cand)) {
                        newOwner = cand;
                        break;
                    }
                }
                if (newOwner == 0)
                    newOwner = room.members.begin()->memberId;
                room.ownerMemberId = newOwner;
                if (RoomMember* om = room.findById(newOwner))
                    om->flagAttr |= Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
                qInfo() << "Room" << roomId << "owner -> mid" << newOwner << "(disconnect)";
            }
        }
    }

    ResetMatchingRoomState(roomId);

    if (!roomDestroyed) {
        shadnet::NotifyMemberLeft pb;
        pb.set_room_id(roomId);
        pb.set_member_id(myMemberId);
        pb.set_npid(m_info.npid.toStdString());
        pb.set_ctx_id(m_matching.ctxId);
        QByteArray payload;
        appendProto(payload, pb);
        NotifyRoomMembers(NotificationType::MemberLeft, payload, m_matching.comId, roomId);
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

    qInfo() << "Matching cleanup for" << m_info.npid;
}
