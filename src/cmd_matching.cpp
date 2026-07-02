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

bool RoomMatchesFilters(const Room& room, const shadnet::SearchRoomRequest& req) {
    // Hidden rooms never appear in searches.
    if (room.flagAttr & Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_HIDDEN)
        return false;

    // NAT_TYPE_RESTRICTION is ignored in the flag comparison (matches RPCN; it is
    // irrelevant here and breaks some titles' searches).
    const uint32_t nat = Matching2::ORBIS_NP_MATCHING2_ROOM_FLAG_ATTR_NAT_TYPE_RESTRICTION;
    const uint32_t flagFilter = req.flag_filter() & ~nat;
    const uint32_t flagAttr = req.flag_attr() & ~nat;
    if ((room.flagAttr & flagFilter) != flagAttr) {
        qWarning() << "  filter reject FLAG: roomFlags=" << Qt::hex << room.flagAttr
                   << "filter=" << flagFilter << "attr=" << flagAttr;
        return false;
    }

    constexpr uint16_t INT_1 = Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_1_ID;
    constexpr uint16_t INT_8 = Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_8_ID;
    for (int i = 0; i < req.int_filters_size(); ++i) {
        const auto& f = req.int_filters(i);
        const uint16_t id = static_cast<uint16_t>(f.attr_id());
        // An empty/unset filter slot (id 0) is sent by some games — skip it.
        if (id == 0)
            continue;
        if (id < INT_1 || id > INT_8) {
            qWarning() << "  filter reject INT id out of range:" << id;
            return false;
        }
        const auto& slot = room.searchIntAttr[id - INT_1];
        if (!MatchIntFilter(f.op(), slot.value, f.attr_value())) {
            qWarning() << "  filter reject INT: id=" << id << "op=" << f.op()
                       << "roomVal=" << slot.value << "set=" << slot.set
                       << "filterVal=" << f.attr_value();
            return false;
        }
    }

    for (int i = 0; i < req.bin_filters_size(); ++i) {
        const auto& f = req.bin_filters(i);
        if (f.attr_id() == 0)
            continue;
        if (static_cast<uint16_t>(f.attr_id()) !=
            Matching2::ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_BIN_ATTR_EXTERNAL_1_ID) {
            qWarning() << "  filter reject BIN id:" << f.attr_id();
            return false;
        }
        const QByteArray fdata(f.data().data(), static_cast<int>(f.data().size()));
        const bool eq = room.searchBinAttr.data == fdata;
        if ((f.op() == 1 && !eq) || (f.op() == 2 && eq)) {
            qWarning() << "  filter reject BIN: op=" << f.op() << "set=" << room.searchBinAttr.set
                       << "roomLen=" << room.searchBinAttr.data.size()
                       << "filterLen=" << fdata.size();
            return false;
        }
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
        a->set_update_date(slot.updateDate);
        a->set_update_member_id(slot.updateMemberId);
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
    pb.set_join_date(member.joinDate);
    pb.set_nat_type(member.natType);
    pb.set_flag_attr(member.flagAttr);
    pb.set_addr(member.addr.toStdString());
    pb.set_port(member.port);
    pb.set_group_id(member.groupId);
    pb.set_account_id(static_cast<uint64_t>(member.userId));
    pb.set_platform(member.platform);
    if (member.memberBinAttr.set) {
        auto* a = pb.add_bin_attrs_internal();
        a->set_attr_id(member.memberBinAttr.attrId);
        a->set_data(member.memberBinAttr.data.constData(), member.memberBinAttr.data.size());
        a->set_update_date(member.memberBinAttr.updateDate);
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
                                      const QString& matchingKey, uint64_t roomId,
                                      const QString& excludeNpid) {
    QVector<std::function<void(QByteArray)>> senders;
    {
        QReadLocker roomLk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({matchingKey, roomId});
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

void ClientSession::SendRoomMemberEvent(uint64_t roomId, uint32_t event, uint32_t cause,
                                        const RoomMember& member, const QString& excludeNpid) {
    shadnet::NotifyRoomEvent pb;
    pb.set_ctx_id(m_matching.ctxId);
    pb.set_room_id(roomId);
    pb.set_event(event);
    pb.set_event_cause(cause);
    *pb.mutable_member() = BuildRoomMemberData(member);
    QByteArray payload;
    appendProto(payload, pb);
    NotifyRoomMembers(NotificationType::RoomEvent, payload, m_matching.matchingKey, roomId,
                      excludeNpid);
}

void ClientSession::SendRoomEventToTarget(uint64_t roomId, uint32_t event, uint32_t cause,
                                          int32_t errorCode, const QString& targetNpid) {
    shadnet::NotifyRoomEvent pb;
    pb.set_ctx_id(m_matching.ctxId);
    pb.set_room_id(roomId);
    pb.set_event(event);
    pb.set_event_cause(cause);
    pb.set_error_code(errorCode);
    QByteArray payload;
    appendProto(payload, pb);
    SendMatchingNotification(NotificationType::RoomEvent, payload, targetNpid);
}

void ClientSession::GetSelfSignalingAddr(QString& addr, uint16_t& port) const {
    addr.clear();
    port = 0;
    {
        QReadLocker lk(&m_shared->matching.udpLock);
        auto it = m_shared->matching.udpExt.find(m_info.npid);
        if (it != m_shared->matching.udpExt.end()) {
            addr = it.value().first;
            port = it.value().second;
        }
    }
    if (addr.isEmpty() && m_socket)
        addr = m_socket->peerAddress().toString();
}

void ClientSession::ResetMatchingRoomState(uint64_t roomId) {
    if (roomId == 0 || m_matching.roomId != roomId)
        return;
    ResetLocalMatchingRoomState(m_matching);
    qInfo() << "Matching room state reset for" << m_info.npid << "room=" << roomId;
}

ErrorType ClientSession::CmdContextStart(StreamExtractor& data) {
    shadnet::ContextStartRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    m_matching.ctxId = req.ctx_id();
    m_matching.initialized = true;

    qInfo() << "ContextStart:" << m_info.npid << "ctx=" << m_matching.ctxId
            << "title=" << m_matching.titleId << "key=" << m_matching.matchingKey;

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdContextStop(StreamExtractor& data) {
    shadnet::ContextStopRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    m_matching.ctxId = 0;
    m_matching.initialized = false;

    qInfo() << "ContextStop:" << m_info.npid;

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdCreateRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::CreateRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

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
    for (const auto& id : req.allowed_online_ids())
        room.allowedUsers.append(QString::fromStdString(id));
    for (const auto& id : req.blocked_online_ids())
        room.blockedUsers.append(QString::fromStdString(id));
    for (const auto id : req.allowed_account_ids())
        room.allowedAccountIds.append(static_cast<uint64_t>(id));
    for (const auto id : req.blocked_account_ids())
        room.blockedAccountIds.append(static_cast<uint64_t>(id));

    if (!req.room_password().empty()) {
        room.roomPassword =
            QByteArray(req.room_password().data(), static_cast<int>(req.room_password().size()));
    } else if (req.room_password_present()) {
        room.roomPassword = QByteArray(Matching2::ORBIS_NP_MATCHING2_SESSION_PASSWORD_SIZE, '\0');
    }
    if (req.has_passwd_slot_mask())
        room.passwdSlotMask = req.passwd_slot_mask();

    const uint32_t groupCount = req.group_configs_size() > 0
                                    ? static_cast<uint32_t>(req.group_configs_size())
                                    : req.group_config_count();
    const QByteArray joinGroupLabel =
        QByteArray(req.join_group_label().data(), static_cast<int>(req.join_group_label().size()));
    for (uint32_t g = 0; g < groupCount; ++g) {
        RoomGroup grp;
        grp.groupId = static_cast<uint8_t>(g + 1);
        if (g < static_cast<uint32_t>(req.group_configs_size())) {
            const auto& cfg = req.group_configs(static_cast<int>(g));
            grp.slotNum = cfg.slot_count();
            grp.withPassword = cfg.has_passwd();
            grp.fixedLabel = cfg.has_label();
            if (cfg.has_label())
                grp.label = QByteArray(cfg.label().data(), static_cast<int>(cfg.label().size()));
        } else {
            grp.withPassword = req.room_password_present();
            grp.fixedLabel = req.join_group_label_present();
            if (!joinGroupLabel.isEmpty())
                grp.label = joinGroupLabel;
        }
        room.groups.append(grp);
    }

    for (int i = 0; i < req.external_search_int_attrs_size(); ++i) {
        const auto& a = req.external_search_int_attrs(i);
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
    if (req.external_search_bin_attrs_size() > 0) {
        const auto& a = req.external_search_bin_attrs(0);
        room.searchBinAttr.set = true;
        room.searchBinAttr.attrId = static_cast<uint16_t>(a.attr_id());
        room.searchBinAttr.data = QByteArray(a.data().data(), static_cast<int>(a.data().size()));
    }
    for (int i = 0; i < req.external_bin_attrs_size(); ++i) {
        const auto& a = req.external_bin_attrs(i);
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

    RoomMember owner;
    owner.userId = m_info.userId;
    owner.npid = m_info.npid;
    owner.avatarUrl = m_info.avatarUrl;
    GetSelfSignalingAddr(owner.addr, owner.port);
    owner.joinDate = MatchingTimestampUsec();
    owner.teamId = static_cast<uint8_t>(req.team_id());
    owner.flagAttr = Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
    if (!room.groups.isEmpty()) {
        auto groupIt = room.groups.begin();
        if (!joinGroupLabel.isEmpty()) {
            for (auto it = room.groups.begin(); it != room.groups.end(); ++it) {
                if (it->label && *it->label == joinGroupLabel) {
                    groupIt = it;
                    break;
                }
            }
        }
        owner.groupId = groupIt->groupId;
        groupIt->numMembers++;
    }
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

    for (int i = 0; i < req.internal_bin_attrs_size(); ++i) {
        const auto& a = req.internal_bin_attrs(i);
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
        slot.updateDate = member->joinDate;
        slot.updateMemberId = memberId;
    }

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

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        const uint32_t worldId = room.worldId;
        const uint64_t lobbyId = room.lobbyId;
        m_shared->matching.rooms.insert({m_matching.matchingKey, rid}, std::move(room));
        if (worldId != 0)
            m_shared->matching.worldRooms[{m_matching.matchingKey, worldId}].append(rid);
        else if (lobbyId != 0)
            m_shared->matching.lobbyRooms[{m_matching.matchingKey, lobbyId}].append(rid);
    }

    qInfo() << "Room" << rid << "created by" << m_info.npid << "max=" << maxSlot
            << "world=" << room.worldId << "lobby=" << room.lobbyId
            << "key=" << m_matching.matchingKey;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdJoinRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::JoinRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();

    uint16_t myMemberId = 0;
    uint16_t maxSlot = 0;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
        if (roomIt == m_shared->matching.rooms.end())
            return ErrorType::RoomMissing;
        Room& room = roomIt.value();
        if (room.isFull())
            return ErrorType::RoomFull;
        if (room.findByNpid(m_info.npid))
            return ErrorType::RoomAlreadyJoined;
        const uint64_t accountId = static_cast<uint64_t>(m_info.userId);
        if ((!room.allowedUsers.isEmpty() && !room.allowedUsers.contains(m_info.npid)) ||
            (!room.allowedAccountIds.isEmpty() && !room.allowedAccountIds.contains(accountId)) ||
            room.blockedUsers.contains(m_info.npid) || room.blockedAccountIds.contains(accountId)) {
            return ErrorType::Blocked;
        }

        RoomMember joiner;
        joiner.userId = m_info.userId;
        joiner.npid = m_info.npid;
        joiner.avatarUrl = m_info.avatarUrl;
        GetSelfSignalingAddr(joiner.addr, joiner.port);
        for (const auto& id : req.blocked_online_ids())
            joiner.blockedUsers.append(QString::fromStdString(id));
        for (const auto id : req.blocked_account_ids())
            joiner.blockedAccountIds.append(static_cast<uint64_t>(id));
        for (const auto& member : room.members) {
            const uint64_t memberAccountId = static_cast<uint64_t>(member.userId);
            if (joiner.blockedUsers.contains(member.npid) ||
                joiner.blockedAccountIds.contains(memberAccountId) ||
                member.blockedUsers.contains(m_info.npid) ||
                member.blockedAccountIds.contains(accountId)) {
                return ErrorType::Blocked;
            }
        }
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
    }

    qInfo() << "Room" << roomId << m_info.npid << "joined mid=" << myMemberId;

    // MemberJoined to existing members (0x1101)
    {
        RoomMember joinedCopy;
        {
            QReadLocker lk(&m_shared->matching.roomsLock);
            auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
            if (roomIt != m_shared->matching.rooms.end()) {
                if (const RoomMember* m = roomIt->findById(myMemberId))
                    joinedCopy = *m;
            }
        }
        SendRoomMemberEvent(roomId, Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_JOINED,
                            Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_SERVER_OPERATION, joinedCopy,
                            m_info.npid);
        qDebug() << "  -> MemberJoined room=" << roomId << "mid=" << myMemberId
                 << "to existing members";
    }

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdLeaveRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::LeaveRoomRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    uint64_t roomId = req.room_id();

    uint16_t myMemberId = 0;
    bool wasOwner = false;
    bool roomDestroyed = false;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
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
                m_shared->matching.worldRooms[{m_matching.matchingKey, worldId}].removeAll(roomId);
            else if (lobbyId != 0)
                m_shared->matching.lobbyRooms[{m_matching.matchingKey, lobbyId}].removeAll(roomId);
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

    // Notify remaining members (MemberLeft, 0x1102)
    if (!roomDestroyed) {
        RoomMember left;
        left.memberId = myMemberId;
        left.npid = m_info.npid;
        SendRoomMemberEvent(roomId, Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT,
                            Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_LEAVE_ACTION, left);
        qDebug() << "  -> MemberLeft room=" << roomId << "npid=" << m_info.npid
                 << "mid=" << myMemberId << "to remaining members";
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
    uint16_t targetMemberId = static_cast<uint16_t>(req.target_member_id());
    uint32_t blockKickFlag = req.block_kick_flag();

    QString targetNpid;

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
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

        targetNpid = targetIt->npid;

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

    // KickedOut to the removed member (0x1103)
    SendRoomEventToTarget(roomId, Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_KICKEDOUT,
                          Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_KICKOUT_ACTION,
                          MATCHING2_KICKEDOUT_STATUS_CODE, targetNpid);
    qDebug() << "  -> KickedOut room=" << roomId << "to" << targetNpid;

    // MemberLeft to remaining members (0x1102, KICKOUT cause)
    {
        RoomMember left;
        left.memberId = targetMemberId;
        left.npid = targetNpid;
        SendRoomMemberEvent(roomId, Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT,
                            Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_KICKOUT_ACTION, left,
                            targetNpid);
        qDebug() << "  -> MemberLeft (kicked) room=" << roomId << "mid=" << targetMemberId;
    }

    shadnet::KickoutRoomMemberReply rep;
    rep.set_room_id(roomId);
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetWorldInfoList(StreamExtractor& data, QByteArray& reply) {
    shadnet::GetWorldInfoListRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const uint16_t serverId = static_cast<uint16_t>(req.server_id());

    shadnet::GetWorldInfoListReply rep;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);

        auto countRoomsAndMembers = [&](uint32_t worldId, uint32_t& roomsNum,
                                        uint32_t& roomMembersNum) {
            roomsNum = 0;
            roomMembersNum = 0;
            const QVector<uint64_t> ids =
                m_shared->matching.worldRooms.value({m_matching.matchingKey, worldId});
            for (uint64_t rid : ids) {
                auto it = m_shared->matching.rooms.find({m_matching.matchingKey, rid});
                if (it == m_shared->matching.rooms.end())
                    continue;
                ++roomsNum;
                roomMembersNum += static_cast<uint32_t>(it.value().members.size());
            }
        };

        const QVector<WorldConfig> configs =
            m_shared->matching.worldConfigs.value(m_matching.matchingKey);

        if (!configs.isEmpty()) {
            for (const WorldConfig& wc : configs) {
                if (serverId != 0 && wc.serverId != serverId)
                    continue;
                uint32_t roomsNum = 0;
                uint32_t roomMembersNum = 0;
                countRoomsAndMembers(wc.worldId, roomsNum, roomMembersNum);

                shadnet::MatchingWorld* w = rep.add_worlds();
                w->set_world_id(wc.worldId);
                w->set_lobbies_num(wc.lobbiesNum);
                w->set_max_lobby_members(wc.maxLobbyMembersNum);
                w->set_lobby_members_num(0);
                w->set_rooms_num(roomsNum);
                w->set_room_members_num(roomMembersNum);
            }
        } else {
            QVector<uint32_t> worldIds;
            for (auto it = m_shared->matching.worldRooms.constBegin();
                 it != m_shared->matching.worldRooms.constEnd(); ++it) {
                if (it.key().first != m_matching.matchingKey)
                    continue;
                const uint32_t worldId = it.key().second;
                if (!worldIds.contains(worldId))
                    worldIds.append(worldId);
            }
            if (worldIds.isEmpty())
                worldIds.append(1);

            for (uint32_t worldId : worldIds) {
                uint32_t roomsNum = 0;
                uint32_t roomMembersNum = 0;
                countRoomsAndMembers(worldId, roomsNum, roomMembersNum);

                shadnet::MatchingWorld* w = rep.add_worlds();
                w->set_world_id(worldId);
                w->set_lobbies_num(0);
                w->set_max_lobby_members(0);
                w->set_lobby_members_num(0);
                w->set_rooms_num(roomsNum);
                w->set_room_members_num(roomMembersNum);
            }
        }
    }

    appendProto(reply, rep);
    qInfo() << "GetWorldInfoList:" << m_info.npid << "server=" << serverId
            << "worlds=" << rep.worlds_size();
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdSearchRoom(StreamExtractor& data, QByteArray& reply) {
    shadnet::SearchRoomRequest req;
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

    shadnet::SearchRoomReply rep;
    uint32_t total = 0;
    int candidateCount = 0;
    {
        QReadLocker lk(&m_shared->matching.roomsLock);

        QVector<uint64_t> candidates;
        if (worldId != 0)
            candidates = m_shared->matching.worldRooms.value({m_matching.matchingKey, worldId});
        else if (lobbyId != 0)
            candidates = m_shared->matching.lobbyRooms.value({m_matching.matchingKey, lobbyId});
        candidateCount = candidates.size();

        QVector<const Room*> matches;
        for (uint64_t rid : candidates) {
            auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, rid});
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

    qInfo() << "SearchRoom:" << m_info.npid << "world=" << worldId << "lobby=" << lobbyId
            << "key=" << m_matching.matchingKey << "candidates=" << candidateCount
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
            if (it.key().first != m_matching.matchingKey)
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
            if (it.key().first != m_matching.matchingKey)
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

    uint64_t roomId = req.room_id();
    uint32_t flagFilter = req.flag_filter();
    uint32_t flagAttr = req.flag_attr();
    bool hasPasswdMask = req.has_passwd_mask();
    uint64_t passwdSlotMask = req.passwd_slot_mask();

    uint32_t newFlags = 0;
    QVector<InternalBinAttrSlot> updatedSlots;
    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
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
            updatedSlots.append(slot);
        }

        qInfo() << "SetRoomDataInternal: room=" << roomId << "flags=" << Qt::hex << newFlags
                << "binAttrs=" << req.bin_attrs_size();
    }

    // Broadcast UpdatedRoomDataInternal to all other members (0x1106)
    {
        shadnet::NotifyRoomEvent pb;
        pb.set_ctx_id(m_matching.ctxId);
        pb.set_room_id(roomId);
        pb.set_event(Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_ROOM_DATA_INTERNAL);
        pb.set_event_cause(Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_SERVER_OPERATION);
        pb.set_flags(newFlags);
        if (hasPasswdMask) {
            pb.set_has_passwd_mask(true);
            pb.set_passwd_slot_mask(passwdSlotMask);
        }
        for (const auto& slot : updatedSlots) {
            auto* ba = pb.add_bin_attrs();
            ba->set_attr_id(slot.attrId);
            ba->set_data(slot.data.constData(), slot.data.size());
            ba->set_update_date(slot.updateDate);
            ba->set_update_member_id(slot.updateMemberId);
        }
        QByteArray notifPayload;
        appendProto(notifPayload, pb);
        NotifyRoomMembers(NotificationType::RoomEvent, notifPayload, m_matching.matchingKey, roomId,
                          m_info.npid);
        qDebug() << "  -> UpdatedRoomDataInternal room=" << roomId << "broadcast (excl. sender)"
                 << "attrs=" << updatedSlots.size();
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

    uint64_t roomId = req.room_id();

    {
        QWriteLocker lk(&m_shared->matching.roomsLock);
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
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
        auto roomIt = m_shared->matching.rooms.find({m_matching.matchingKey, roomId});
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
                m_shared->matching.worldRooms[{m_matching.matchingKey, worldId}].removeAll(roomId);
            else if (lobbyId != 0)
                m_shared->matching.lobbyRooms[{m_matching.matchingKey, lobbyId}].removeAll(roomId);
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
        RoomMember left;
        left.memberId = myMemberId;
        left.npid = m_info.npid;
        SendRoomMemberEvent(roomId, Matching2::ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT,
                            Matching2::ORBIS_NP_MATCHING2_EVENT_CAUSE_LEAVE_ACTION, left);
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
