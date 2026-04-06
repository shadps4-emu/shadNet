// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include "client.h"
#include "score.pb.h"

using namespace shadnet;

bool ShadNetClient::connect(const char* host, uint16_t port) {
    if (!conn.connect(host, port))
        return false;
    conn.onPacket = [this](const Packet& pkt) { handlePacket(pkt); };
    return true;
}

void ShadNetClient::disconnect() {
    conn.disconnect();
}
void ShadNetClient::update() {
    conn.update();
}

std::vector<uint8_t> ShadNetClient::buildPacket(CommandType cmd, uint64_t id,
                                                const std::vector<uint8_t>& payload) {
    uint32_t totalSize = static_cast<uint32_t>(HEADER_SIZE + payload.size());
    std::vector<uint8_t> out(HEADER_SIZE);
    out[0] = static_cast<uint8_t>(PacketType::Request);
    uint16_t cmdLE = toLE16(static_cast<uint16_t>(cmd));
    uint32_t sizeLE = toLE32(totalSize);
    uint64_t idLE = toLE64(id);
    memcpy(out.data() + 1, &cmdLE, 2);
    memcpy(out.data() + 3, &sizeLE, 4);
    memcpy(out.data() + 7, &idLE, 8);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Account commands
void ShadNetClient::login(const std::string& npid, const std::string& password,
                          const std::string& token) {
    std::vector<uint8_t> payload;
    auto addStr = [&](const std::string& s) {
        payload.insert(payload.end(), s.begin(), s.end());
        payload.push_back(0);
    };
    addStr(npid);
    addStr(password);
    addStr(token);
    conn.send(buildPacket(CommandType::Login, packetCounter++, payload));
}

void ShadNetClient::createAccount(const std::string& npid, const std::string& password,
                                  const std::string& onlineName, const std::string& avatarUrl,
                                  const std::string& email) {
    std::vector<uint8_t> payload;
    auto addStr = [&](const std::string& s) {
        payload.insert(payload.end(), s.begin(), s.end());
        payload.push_back(0);
    };
    addStr(npid);
    addStr(password);
    addStr(onlineName);
    addStr(avatarUrl);
    addStr(email);
    conn.send(buildPacket(CommandType::Create, packetCounter++, payload));
}

// Friend commands

void ShadNetClient::sendFriendCommand(CommandType cmd, const std::string& targetNpid) {
    m_pendingFriendCmd = cmd;
    m_pendingFriendNpid = targetNpid;
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), targetNpid.begin(), targetNpid.end());
    payload.push_back(0);
    conn.send(buildPacket(cmd, packetCounter++, payload));
}

void ShadNetClient::addFriend(const std::string& n) {
    sendFriendCommand(CommandType::AddFriend, n);
}
void ShadNetClient::removeFriend(const std::string& n) {
    sendFriendCommand(CommandType::RemoveFriend, n);
}
void ShadNetClient::addBlock(const std::string& n) {
    sendFriendCommand(CommandType::AddBlock, n);
}
void ShadNetClient::removeBlock(const std::string& n) {
    sendFriendCommand(CommandType::RemoveBlock, n);
}

// NPScore wire helpers

// Build the standard score request payload:
//   ComId(12 bytes, padded) + u32 LE blob size + serialised protobuf
static std::vector<uint8_t> makeScorePayload(const std::string& comId,
                                             const google::protobuf::MessageLite& msg) {
    std::vector<uint8_t> out(12, 0);
    size_t n = (std::min)(comId.size(), size_t(12));
    memcpy(out.data(), comId.data(), n);

    std::string serialised;
    msg.SerializeToString(&serialised);
    uint32_t len = static_cast<uint32_t>(serialised.size());
    uint8_t lb[4];
    memcpy(lb, &len, 4);
    out.insert(out.end(), lb, lb + 4);
    out.insert(out.end(), serialised.begin(), serialised.end());
    return out;
}

// Read the u32 LE-prefixed blob at the given position and return it as a std::string
// ready for ParseFromString.
static std::string extractBlob(const std::vector<uint8_t>& p, int pos) {
    if (pos + 4 > static_cast<int>(p.size()))
        return {};
    uint32_t len = 0;
    memcpy(&len, p.data() + pos, 4);
    pos += 4;
    if (pos + static_cast<int>(len) > static_cast<int>(p.size()))
        return {};
    return std::string(reinterpret_cast<const char*>(p.data() + pos), len);
}

// NPScore send methods
void ShadNetClient::getBoardInfos(const std::string& comId, uint32_t boardId) {
    // GetBoardInfos is special: boardId is a raw u32 LE, not a protobuf blob.
    std::vector<uint8_t> payload(12, 0);
    size_t n = (std::min)(comId.size(), size_t(12));
    memcpy(payload.data(), comId.data(), n);
    uint8_t id[4];
    memcpy(id, &boardId, 4);
    payload.insert(payload.end(), id, id + 4);
    conn.send(buildPacket(CommandType::GetBoardInfos, packetCounter++, payload));
}

void ShadNetClient::recordScore(const std::string& comId, uint32_t boardId, int32_t pcId,
                                int64_t score, const std::string& comment,
                                const std::vector<uint8_t>& gameInfo) {
    score::RecordScoreRequest req;
    req.set_boardid(boardId);
    req.set_pcid(pcId);
    req.set_score(score);
    if (!comment.empty())
        req.set_comment(comment);
    if (!gameInfo.empty())
        req.set_data(gameInfo.data(), gameInfo.size());
    conn.send(buildPacket(CommandType::RecordScore, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::recordScoreData(const std::string& comId, uint32_t boardId, int32_t pcId,
                                    int64_t score, const std::vector<uint8_t>& data) {
    score::RecordScoreGameDataRequest req;
    req.set_boardid(boardId);
    req.set_pcid(pcId);
    req.set_score(score);

    // Wire: ComId(12) + pb_blob(u32+bytes) + raw_data(u32+bytes)
    std::vector<uint8_t> payload = makeScorePayload(comId, req);
    uint32_t dlen = static_cast<uint32_t>(data.size());
    uint8_t dl[4];
    memcpy(dl, &dlen, 4);
    payload.insert(payload.end(), dl, dl + 4);
    payload.insert(payload.end(), data.begin(), data.end());
    conn.send(buildPacket(CommandType::RecordScoreData, packetCounter++, payload));
}

void ShadNetClient::getScoreData(const std::string& comId, uint32_t boardId,
                                 const std::string& npid, int32_t pcId) {
    score::GetScoreGameDataRequest req;
    req.set_boardid(boardId);
    req.set_npid(npid);
    req.set_pcid(pcId);
    conn.send(
        buildPacket(CommandType::GetScoreData, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreRange(const std::string& comId, uint32_t boardId, uint32_t startRank,
                                  uint32_t numRanks, bool withComment, bool withGameInfo) {
    score::GetScoreRangeRequest req;
    req.set_boardid(boardId);
    req.set_startrank(startRank);
    req.set_numranks(numRanks);
    req.set_withcomment(withComment);
    req.set_withgameinfo(withGameInfo);
    conn.send(
        buildPacket(CommandType::GetScoreRange, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreNpid(const std::string& comId, uint32_t boardId,
                                 const std::vector<NpIdPcId>& targets, bool withComment,
                                 bool withGameInfo) {
    score::GetScoreNpIdRequest req;
    req.set_boardid(boardId);
    for (const auto& t : targets) {
        auto* e = req.add_npids();
        e->set_npid(t.npid);
        e->set_pcid(t.pcId);
    }
    req.set_withcomment(withComment);
    req.set_withgameinfo(withGameInfo);
    conn.send(
        buildPacket(CommandType::GetScoreNpid, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreFriends(const std::string& comId, uint32_t boardId, bool includeSelf,
                                    uint32_t max, bool withComment, bool withGameInfo) {
    score::GetScoreFriendsRequest req;
    req.set_boardid(boardId);
    req.set_includeself(includeSelf);
    req.set_max(max);
    req.set_withcomment(withComment);
    req.set_withgameinfo(withGameInfo);
    conn.send(
        buildPacket(CommandType::GetScoreFriends, packetCounter++, makeScorePayload(comId, req)));
}

// Reply handlers

static bool skipPresence(const std::vector<uint8_t>& p, int& pos) {
    if (pos + 12 > static_cast<int>(p.size()))
        return false;
    pos += 12;
    for (int i = 0; i < 3; ++i) {
        while (pos < static_cast<int>(p.size()) && p[pos] != 0)
            ++pos;
        if (pos >= static_cast<int>(p.size()))
            return false;
        ++pos;
    }
    if (pos + 4 > static_cast<int>(p.size()))
        return false;
    uint32_t dlen = fromLE32(p.data() + pos);
    pos += 4 + static_cast<int>(dlen);
    return pos <= static_cast<int>(p.size());
}

void ShadNetClient::handleLoginReply(const std::vector<uint8_t>& payload) {
    LoginResult res;
    if (payload.empty()) {
        res.error = ErrorType::Malformed;
    } else {
        res.error = static_cast<ErrorType>(payload[0]);
        if (res.error == ErrorType::NoError) {
            int pos = 1;
            auto readStr = [&]() -> std::string {
                std::string s;
                while (pos < static_cast<int>(payload.size()) && payload[pos] != 0)
                    s += static_cast<char>(payload[pos++]);
                if (pos < static_cast<int>(payload.size()))
                    ++pos;
                return s;
            };
            auto readU32 = [&]() -> uint32_t {
                if (pos + 4 > static_cast<int>(payload.size()))
                    return 0;
                uint32_t v = fromLE32(payload.data() + pos);
                pos += 4;
                return v;
            };
            res.onlineName = readStr();
            res.avatarUrl = readStr();
            if (pos + 8 <= static_cast<int>(payload.size())) {
                res.userId = fromLE64(payload.data() + pos);
                pos += 8;
            }
            uint32_t fc = readU32();
            for (uint32_t i = 0; i < fc && pos < static_cast<int>(payload.size()); ++i) {
                FriendEntry fe;
                fe.npid = readStr();
                fe.online = (pos < static_cast<int>(payload.size())) && (payload[pos++] != 0);
                skipPresence(payload, pos);
                res.friends.push_back(fe);
            }
            uint32_t sc = readU32();
            for (uint32_t i = 0; i < sc; ++i)
                res.friendRequestsSent.push_back(readStr());
            uint32_t rc = readU32();
            for (uint32_t i = 0; i < rc; ++i)
                res.friendRequestsReceived.push_back(readStr());
            uint32_t bc = readU32();
            for (uint32_t i = 0; i < bc; ++i)
                res.blocked.push_back(readStr());
        }
    }

    if (res.error == ErrorType::NoError) {
        printf("[login] OK  onlineName=%s  userId=%llu\n", res.onlineName.c_str(),
               static_cast<unsigned long long>(res.userId));
        printf("[login]   friends(%zu):", res.friends.size());
        for (const auto& f : res.friends)
            printf("  %s(%s)", f.npid.c_str(), f.online ? "online" : "offline");
        printf(res.friends.empty() ? "  (none)\n" : "\n");
        if (!res.friendRequestsSent.empty()) {
            printf("[login]   requests_sent:");
            for (const auto& n : res.friendRequestsSent)
                printf("  %s", n.c_str());
            printf("\n");
        }
        if (!res.friendRequestsReceived.empty()) {
            printf("[login]   requests_received:");
            for (const auto& n : res.friendRequestsReceived)
                printf("  %s", n.c_str());
            printf("\n");
        }
        if (!res.blocked.empty()) {
            printf("[login]   blocked:");
            for (const auto& n : res.blocked)
                printf("  %s", n.c_str());
            printf("\n");
        }
    } else {
        printf("[login] FAILED: %s\n", errorName(res.error));
    }
    if (onLoginResult)
        onLoginResult(res);
}

void ShadNetClient::handleCreateReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    if (err == ErrorType::NoError)
        printf("[create] Account created successfully.\n");
    else
        printf("[create] FAILED: %s\n", errorName(err));
    if (onCreateResult)
        onCreateResult(err);
}

void ShadNetClient::handleFriendReply(CommandType cmd, const std::vector<uint8_t>& payload) {
    static const char* names[] = {"login",          "terminate",   "create",
                                  "delete",         "send-token",  "send-reset-token",
                                  "reset-password", "reset-state", "add-friend",
                                  "remove-friend",  "add-block",   "remove-block"};
    int idx = static_cast<int>(cmd);
    const char* name = (idx >= 0 && idx < 12) ? names[idx] : "friend-cmd";

    FriendResult res;
    res.cmd = cmd;
    res.targetNpid = m_pendingFriendNpid;
    res.error = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError)
        printf("[%s] OK  target=%s\n", name, res.targetNpid.c_str());
    else
        printf("[%s] FAILED: %s  target=%s\n", name, errorName(res.error), res.targetNpid.c_str());
    if (onFriendResult)
        onFriendResult(res);
}

void ShadNetClient::handleNotification(const Packet& pkt) {
    const auto& p = pkt.payload;
    int pos = 0;
    auto readStr = [&]() -> std::string {
        std::string s;
        while (pos < static_cast<int>(p.size()) && p[pos] != 0)
            s += static_cast<char>(p[pos++]);
        if (pos < static_cast<int>(p.size()))
            ++pos;
        return s;
    };

    switch (static_cast<NotificationType>(pkt.command)) {
    case NotificationType::FriendQuery: {
        NotifyFriendQuery n;
        n.fromNpid = readStr();
        printf("[notify] FriendQuery from %s\n", n.fromNpid.c_str());
        if (onFriendQuery)
            onFriendQuery(n);
        break;
    }
    case NotificationType::FriendNew: {
        NotifyFriendNew n;
        if (pos < static_cast<int>(p.size()))
            n.online = (p[pos++] != 0);
        n.npid = readStr();
        printf("[notify] FriendNew %s (%s)\n", n.npid.c_str(), n.online ? "online" : "offline");
        if (onFriendNew)
            onFriendNew(n);
        break;
    }
    case NotificationType::FriendLost: {
        NotifyFriendLost n;
        n.npid = readStr();
        printf("[notify] FriendLost %s\n", n.npid.c_str());
        if (onFriendLost)
            onFriendLost(n);
        break;
    }
    case NotificationType::FriendStatus: {
        NotifyFriendStatus n;
        if (pos < static_cast<int>(p.size()))
            n.online = (p[pos++] != 0);
        if (pos + 8 <= static_cast<int>(p.size())) {
            n.timestamp = fromLE64(p.data() + pos);
            pos += 8;
        }
        n.npid = readStr();
        printf("[notify] FriendStatus %s is %s\n", n.npid.c_str(), n.online ? "online" : "offline");
        if (onFriendStatus)
            onFriendStatus(n);
        break;
    }
    default:
        printf("[notify] Unknown type=%u\n", pkt.command);
        break;
    case NotificationType::RequestEvent:
        handleNotifyRequestEvent(pkt.payload);
        break;
    case NotificationType::MemberJoined:
        handleNotifyMemberJoined(pkt.payload);
        break;
    case NotificationType::MemberLeft:
        handleNotifyMemberLeft(pkt.payload);
        break;
    case NotificationType::SignalingHelper:
        handleNotifySignalingHelper(pkt.payload);
        break;
    case NotificationType::SignalingEvent:
        handleNotifySignalingEvent(pkt.payload);
        break;
    case NotificationType::NpSignalingEvent:
        handleNotifyNpSignalingEvent(pkt.payload);
        break;
    case NotificationType::RoomDataInternalUpdated:
        handleNotifyRoomDataIntUpdated(pkt.payload);
        break;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Matching — send methods
// ══════════════════════════════════════════════════════════════════════════════

void ShadNetClient::registerHandlers(const std::string& addr, uint16_t port, uint32_t ctxId,
                                     uint32_t serviceLabel, uint8_t handlers) {
    std::vector<uint8_t> payload;
    // addr\0
    payload.insert(payload.end(), addr.begin(), addr.end());
    payload.push_back(0);
    // port (u16 LE)
    payload.push_back(port & 0xFF);
    payload.push_back((port >> 8) & 0xFF);
    // ctxId (u32 LE)
    for (int i = 0; i < 4; ++i)
        payload.push_back((ctxId >> (8 * i)) & 0xFF);
    // serviceLabel (u32 LE)
    for (int i = 0; i < 4; ++i)
        payload.push_back((serviceLabel >> (8 * i)) & 0xFF);
    // handlerCount
    constexpr uint8_t COUNT = 7;
    payload.push_back(COUNT);
    // For each handler slot: enabled(u8) + callbackAddr(u64) + callbackArg(u64)
    for (uint8_t i = 0; i < COUNT; ++i) {
        bool enabled = (handlers >> i) & 1;
        payload.push_back(enabled ? 1 : 0);
        for (int j = 0; j < 8; ++j)
            payload.push_back(0); // callbackAddr
        for (int j = 0; j < 8; ++j)
            payload.push_back(0); // callbackArg
    }
    conn.send(buildPacket(CommandType::RegisterHandlers, packetCounter++, payload));
}

void ShadNetClient::createRoom(uint32_t reqId, uint16_t maxSlots, uint16_t worldId,
                               uint32_t flags) {
    std::vector<uint8_t> p;
    auto u16 = [&](uint16_t v) {
        p.push_back(v & 0xFF);
        p.push_back((v >> 8) & 0xFF);
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            p.push_back((v >> (8 * i)) & 0xFF);
    };
    u32(reqId);
    u16(maxSlots);
    u16(0); // teamId
    u16(worldId);
    u16(0); // lobbyId
    u32(flags);
    u16(1);         // groupConfigCount
    u16(0);         // allowedUserCount
    u16(0);         // blockedUserCount
    u16(0);         // internalBinAttrCount
    u16(0);         // externalSearchIntAttrCount
    u16(0);         // externalSearchBinAttrCount
    u16(0);         // externalBinAttrCount
    u16(0);         // memberInternalBinAttrCount
    p.push_back(0); // joinGroupLabelPresent
    p.push_back(0); // roomPasswordPresent
    p.push_back(0); // signalingType
    p.push_back(0); // signalingFlag
    u16(0);         // signalingMainMember
    conn.send(buildPacket(CommandType::CreateRoom, packetCounter++, p));
}

void ShadNetClient::joinRoom(uint64_t roomId, uint32_t reqId) {
    std::vector<uint8_t> p;
    auto u16 = [&](uint16_t v) {
        p.push_back(v & 0xFF);
        p.push_back((v >> 8) & 0xFF);
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            p.push_back((v >> (8 * i)) & 0xFF);
    };
    auto u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i)
            p.push_back((v >> (8 * i)) & 0xFF);
    };
    u64(roomId);
    u32(reqId);
    u16(0);         // teamId
    u32(0);         // joinFlags
    u16(0);         // blockedUserCount
    u16(0);         // joinMemberBinAttrCount
    p.push_back(0); // roomPasswordPresent
    p.push_back(0); // joinGroupLabelPresent
    conn.send(buildPacket(CommandType::JoinRoom, packetCounter++, p));
}

void ShadNetClient::leaveRoom(uint64_t roomId, uint32_t reqId) {
    std::vector<uint8_t> p;
    for (int i = 0; i < 8; ++i)
        p.push_back((roomId >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i)
        p.push_back((reqId >> (8 * i)) & 0xFF);
    conn.send(buildPacket(CommandType::LeaveRoom, packetCounter++, p));
}

void ShadNetClient::getRoomList() {
    conn.send(buildPacket(CommandType::GetRoomList, packetCounter++, {}));
}

void ShadNetClient::requestSignalingInfos(const std::string& targetNpid) {
    std::vector<uint8_t> p;
    p.insert(p.end(), targetNpid.begin(), targetNpid.end());
    p.push_back(0);
    conn.send(buildPacket(CommandType::RequestSignalingInfos, packetCounter++, p));
}

void ShadNetClient::signalingEstablished(const std::string& peerNpid, uint32_t connId) {
    std::vector<uint8_t> p;
    p.insert(p.end(), peerNpid.begin(), peerNpid.end());
    p.push_back(0);
    for (int i = 0; i < 4; ++i)
        p.push_back((connId >> (8 * i)) & 0xFF);
    conn.send(buildPacket(CommandType::SignalingEstablished, packetCounter++, p));
}

// ── Matching reply handlers ───────────────────────────────────────────────────

void ShadNetClient::handleRegisterHandlersReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    if (err == ErrorType::NoError)
        printf("[register-handlers] OK\n");
    else
        printf("[register-handlers] FAILED: %s\n", errorName(err));
    if (onRegisterHandlers)
        onRegisterHandlers(err);
}

void ShadNetClient::handleCreateRoomReply(const std::vector<uint8_t>& payload) {
    CreateRoomResult res;
    if (payload.empty()) {
        if (onCreateRoom)
            onCreateRoom(res);
        return;
    }
    res.error = static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError && payload.size() >= 19) {
        int pos = 1;
        // CreateRoom reply: roomId(8)+serverId(2)+worldId(2)+lobbyId(2)+memberId(2)+maxSlots(2)+...
        memcpy(&res.roomId, payload.data() + pos, 8);
        pos += 8;
        pos += 2; // serverId
        pos += 2; // worldId
        pos += 2; // lobbyId
        memcpy(&res.memberId, payload.data() + pos, 2);
        pos += 2;
        memcpy(&res.maxSlots, payload.data() + pos, 2);
        printf("[create-room] OK  roomId=%llu memberId=%u maxSlots=%u\n",
               static_cast<unsigned long long>(res.roomId), res.memberId, res.maxSlots);
    } else if (res.error != ErrorType::NoError) {
        printf("[create-room] FAILED: %s\n", errorName(res.error));
    }
    if (onCreateRoom)
        onCreateRoom(res);
}

void ShadNetClient::handleJoinRoomReply(const std::vector<uint8_t>& payload) {
    JoinRoomResult res;
    if (payload.empty()) {
        if (onJoinRoom)
            onJoinRoom(res);
        return;
    }
    res.error = static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError && payload.size() >= 15) {
        int pos = 1;
        memcpy(&res.roomId, payload.data() + pos, 8);
        pos += 8;
        memcpy(&res.memberId, payload.data() + pos, 2);
        pos += 2;
        memcpy(&res.maxSlots, payload.data() + pos, 2);
        pos += 2;
        printf("[join-room] OK  roomId=%llu memberId=%u maxSlots=%u\n",
               static_cast<unsigned long long>(res.roomId), res.memberId, res.maxSlots);
    } else if (res.error != ErrorType::NoError) {
        printf("[join-room] FAILED: %s\n", errorName(res.error));
    }
    if (onJoinRoom)
        onJoinRoom(res);
}

void ShadNetClient::handleLeaveRoomReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    uint64_t roomId = 0;
    if (err == ErrorType::NoError && payload.size() >= 9)
        memcpy(&roomId, payload.data() + 1, 8);
    if (err == ErrorType::NoError)
        printf("[leave-room] OK  roomId=%llu\n", static_cast<unsigned long long>(roomId));
    else
        printf("[leave-room] FAILED: %s\n", errorName(err));
    if (onLeaveRoom)
        onLeaveRoom(err, roomId);
}

void ShadNetClient::handleGetRoomListReply(const std::vector<uint8_t>& payload) {
    RoomListResult res;
    if (payload.empty()) {
        if (onRoomList)
            onRoomList(res);
        return;
    }
    res.error = static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError && payload.size() >= 5) {
        int pos = 1;
        uint32_t count = fromLE32(payload.data() + pos);
        pos += 4;
        printf("[room-list] %u room(s)\n", count);
        for (uint32_t i = 0; i < count && pos + 20 <= (int)payload.size(); ++i) {
            RoomEntry e;
            // Parse minimal RoomDataExternal fields we care about
            memcpy(&e.maxSlots, payload.data() + pos, 2);
            pos += 2;
            memcpy(&e.curMembers, payload.data() + pos, 2);
            pos += 2;
            pos += 4; // flags
            pos += 2; // serverId
            pos += 4; // worldId
            pos += 8; // lobbyId
            memcpy(&e.roomId, payload.data() + pos, 8);
            pos += 8;
            // Skip the rest — we don't need full parsing for sample output
            printf("  room %llu  slots=%u/%u\n", static_cast<unsigned long long>(e.roomId),
                   e.curMembers, e.maxSlots);
            res.rooms.push_back(e);
            // Skip remaining room fields (variable length — stop parsing for simplicity)
            break;
        }
    } else if (res.error != ErrorType::NoError) {
        printf("[room-list] FAILED: %s\n", errorName(res.error));
    }
    if (onRoomList)
        onRoomList(res);
}

void ShadNetClient::handleSignalingInfosReply(const std::vector<uint8_t>& payload) {
    SignalingInfoResult res;
    if (payload.empty()) {
        if (onSignalingInfos)
            onSignalingInfos(res);
        return;
    }
    res.error = static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError) {
        int pos = 1;
        auto readStr = [&]() -> std::string {
            std::string s;
            while (pos < (int)payload.size() && payload[pos] != 0)
                s += static_cast<char>(payload[pos++]);
            if (pos < (int)payload.size())
                ++pos;
            return s;
        };
        res.targetNpid = readStr();
        res.targetIp = readStr();
        if (pos + 4 <= (int)payload.size()) {
            memcpy(&res.targetPort, payload.data() + pos, 2);
            pos += 2;
            memcpy(&res.targetMemberId, payload.data() + pos, 2);
        }
        printf("[signaling-infos] OK  %s @ %s:%u (mid=%u)\n", res.targetNpid.c_str(),
               res.targetIp.c_str(), res.targetPort, res.targetMemberId);
    } else {
        printf("[signaling-infos] FAILED: %s\n", errorName(res.error));
    }
    if (onSignalingInfos)
        onSignalingInfos(res);
}

// ── Matching notification handlers ────────────────────────────────────────────

void ShadNetClient::handleNotifyRequestEvent(const std::vector<uint8_t>& p) {
    if (p.size() < 26)
        return;
    int pos = 0;
    NotifyRequestEvent n;
    memcpy(&n.ctxId, p.data() + pos, 4);
    pos += 4;
    pos += 6; // serverId(2) + worldId(2) + lobbyId(2)
    memcpy(&n.reqEvent, p.data() + pos, 2);
    pos += 2;
    memcpy(&n.reqId, p.data() + pos, 4);
    pos += 4;
    memcpy(&n.errorCode, p.data() + pos, 4);
    pos += 4;
    memcpy(&n.roomId, p.data() + pos, 8);
    pos += 8;
    memcpy(&n.memberId, p.data() + pos, 2);
    pos += 2;
    memcpy(&n.maxSlots, p.data() + pos, 2);
    pos += 2;
    pos += 4; // flags
    n.isOwner = (pos < (int)p.size()) && (p[pos] != 0);
    printf("[notify] RequestEvent reqEvent=0x%04x roomId=%llu memberId=%u owner=%s\n", n.reqEvent,
           static_cast<unsigned long long>(n.roomId), n.memberId, n.isOwner ? "yes" : "no");
    if (onRequestEvent)
        onRequestEvent(n);
}

void ShadNetClient::handleNotifyMemberJoined(const std::vector<uint8_t>& p) {
    if (p.size() < 12)
        return;
    int pos = 0;
    NotifyMemberJoined n;
    memcpy(&n.roomId, p.data() + pos, 8);
    pos += 8;
    memcpy(&n.memberId, p.data() + pos, 2);
    pos += 2;
    auto readStr = [&]() -> std::string {
        std::string s;
        while (pos < (int)p.size() && p[pos] != 0)
            s += static_cast<char>(p[pos++]);
        if (pos < (int)p.size())
            ++pos;
        return s;
    };
    n.npid = readStr();
    n.addr = readStr();
    if (pos + 2 <= (int)p.size()) {
        memcpy(&n.port, p.data() + pos, 2);
        pos += 2;
    }
    printf("[notify] MemberJoined roomId=%llu memberId=%u npid=%s addr=%s:%u\n",
           static_cast<unsigned long long>(n.roomId), n.memberId, n.npid.c_str(), n.addr.c_str(),
           n.port);
    if (onMemberJoined)
        onMemberJoined(n);
}

void ShadNetClient::handleNotifyMemberLeft(const std::vector<uint8_t>& p) {
    if (p.size() < 10)
        return;
    int pos = 0;
    NotifyMemberLeft n;
    memcpy(&n.roomId, p.data() + pos, 8);
    pos += 8;
    memcpy(&n.memberId, p.data() + pos, 2);
    pos += 2;
    while (pos < (int)p.size() && p[pos] != 0)
        n.npid += static_cast<char>(p[pos++]);
    printf("[notify] MemberLeft roomId=%llu memberId=%u npid=%s\n",
           static_cast<unsigned long long>(n.roomId), n.memberId, n.npid.c_str());
    if (onMemberLeft)
        onMemberLeft(n);
}

void ShadNetClient::handleNotifySignalingHelper(const std::vector<uint8_t>& p) {
    int pos = 0;
    auto readStr = [&]() -> std::string {
        std::string s;
        while (pos < (int)p.size() && p[pos] != 0)
            s += static_cast<char>(p[pos++]);
        if (pos < (int)p.size())
            ++pos;
        return s;
    };
    NotifySignalingHelper n;
    n.peerNpid = readStr();
    if (pos + 2 <= (int)p.size()) {
        memcpy(&n.peerMemberId, p.data() + pos, 2);
        pos += 2;
    }
    n.peerAddr = readStr();
    if (pos + 2 <= (int)p.size())
        memcpy(&n.peerPort, p.data() + pos, 2);
    printf("[notify] SignalingHelper peer=%s (mid=%u) @ %s:%u\n", n.peerNpid.c_str(),
           n.peerMemberId, n.peerAddr.c_str(), n.peerPort);
    if (onSignalingHelper)
        onSignalingHelper(n);
}

void ShadNetClient::handleNotifySignalingEvent(const std::vector<uint8_t>& p) {
    if (p.size() < 16)
        return;
    int pos = 0;
    NotifySignalingEvent n;
    memcpy(&n.event, p.data() + pos, 2);
    pos += 2;
    memcpy(&n.roomId, p.data() + pos, 8);
    pos += 8;
    memcpy(&n.memberId, p.data() + pos, 2);
    pos += 2;
    memcpy(&n.connId, p.data() + pos, 4);
    printf("[notify] SignalingEvent event=0x%04x roomId=%llu memberId=%u\n", n.event,
           static_cast<unsigned long long>(n.roomId), n.memberId);
    if (onSignalingEvent)
        onSignalingEvent(n);
}

void ShadNetClient::handleNotifyNpSignalingEvent(const std::vector<uint8_t>& p) {
    if (p.size() < 5)
        return;
    int pos = 0;
    NotifyNpSignalingEvent n;
    memcpy(&n.event, p.data() + pos, 4);
    pos += 4;
    while (pos < (int)p.size() && p[pos] != 0)
        n.peerNpid += static_cast<char>(p[pos++]);
    printf("[notify] NpSignalingEvent event=%u peer=%s\n", n.event, n.peerNpid.c_str());
    if (onNpSignalingEvent)
        onNpSignalingEvent(n);
}

void ShadNetClient::handleNotifyRoomDataIntUpdated(const std::vector<uint8_t>& p) {
    if (p.size() < 14)
        return;
    int pos = 0;
    NotifyRoomDataInternalUpdated n;
    memcpy(&n.roomId, p.data() + pos, 8);
    pos += 8;
    memcpy(&n.flags, p.data() + pos, 4);
    pos += 4;
    uint16_t count = 0;
    memcpy(&count, p.data() + pos, 2);
    pos += 2;
    for (uint16_t i = 0; i < count && pos + 6 <= (int)p.size(); ++i) {
        NotifyRoomDataInternalUpdated::BinAttr a;
        memcpy(&a.attrId, p.data() + pos, 2);
        pos += 2;
        uint32_t sz = 0;
        memcpy(&sz, p.data() + pos, 4);
        pos += 4;
        if (pos + (int)sz <= (int)p.size()) {
            a.data.assign(p.begin() + pos, p.begin() + pos + sz);
            pos += sz;
        }
        n.binAttrs.push_back(std::move(a));
    }
    printf("[notify] RoomDataInternalUpdated roomId=%llu flags=0x%x attrs=%zu\n",
           static_cast<unsigned long long>(n.roomId), n.flags, n.binAttrs.size());
    if (onRoomDataInternalUpdated)
        onRoomDataInternalUpdated(n);
}

void ShadNetClient::handlePacket(const Packet& pkt) {
    if (pkt.type == PacketType::Notify) {
        handleNotification(pkt);
        return;
    }
    if (pkt.type == PacketType::ServerInfo)
        return;
    if (pkt.type != PacketType::Reply)
        return;

    switch (static_cast<CommandType>(pkt.command)) {
    case CommandType::Login:
        handleLoginReply(pkt.payload);
        break;
    case CommandType::Create:
        handleCreateReply(pkt.payload);
        break;
    case CommandType::AddFriend:
    case CommandType::RemoveFriend:
    case CommandType::AddBlock:
    case CommandType::RemoveBlock:
        handleFriendReply(static_cast<CommandType>(pkt.command), pkt.payload);
        break;
    case CommandType::GetBoardInfos:
        handleBoardInfosReply(pkt.payload);
        break;
    case CommandType::RecordScore:
        handleRecordScoreReply(pkt.payload);
        break;
    case CommandType::RecordScoreData:
        handleRecordScoreDataReply(pkt.payload);
        break;
    case CommandType::GetScoreData:
        handleGetScoreDataReply(pkt.payload);
        break;
    case CommandType::GetScoreRange:
        handleScoreRangeReply(pkt.payload, onScoreRange);
        break;
    case CommandType::GetScoreFriends:
        handleScoreRangeReply(pkt.payload, onScoreFriends);
        break;
    case CommandType::GetScoreNpid:
        handleScoreRangeReply(pkt.payload, onScoreNpid);
        break;
    case CommandType::RegisterHandlers:
        handleRegisterHandlersReply(pkt.payload);
        break;
    case CommandType::CreateRoom:
        handleCreateRoomReply(pkt.payload);
        break;
    case CommandType::JoinRoom:
        handleJoinRoomReply(pkt.payload);
        break;
    case CommandType::LeaveRoom:
        handleLeaveRoomReply(pkt.payload);
        break;
    case CommandType::GetRoomList:
        handleGetRoomListReply(pkt.payload);
        break;
    case CommandType::RequestSignalingInfos:
        handleSignalingInfosReply(pkt.payload);
        break;
    case CommandType::SignalingEstablished:
    case CommandType::ActivationConfirm:
    case CommandType::SetRoomDataInternal:
    case CommandType::SetRoomDataExternal:
        // reply is error byte only — already printed by generic handler
        break;
    default:
        printf("[recv] Unhandled reply command=%u\n", pkt.command);
        break;
    }
}

// NPScore reply handlers

void ShadNetClient::handleBoardInfosReply(const std::vector<uint8_t>& payload) {
    BoardInfo info;
    if (payload.empty()) {
        if (onBoardInfos)
            onBoardInfos(info);
        return;
    }
    info.error = static_cast<ErrorType>(payload[0]);
    if (info.error == ErrorType::NoError) {
        score::BoardInfo pb;
        if (pb.ParseFromString(extractBlob(payload, 1))) {
            info.rankLimit = pb.ranklimit();
            info.updateMode = pb.updatemode();
            info.sortMode = pb.sortmode();
            info.uploadNumLimit = pb.uploadnumlimit();
            info.uploadSizeLimit = pb.uploadsizelimit();
        }
        printf("[board-info] rankLimit=%u updateMode=%u sortMode=%u"
               " uploadNum=%u uploadSize=%u\n",
               info.rankLimit, info.updateMode, info.sortMode, info.uploadNumLimit,
               info.uploadSizeLimit);
    } else {
        printf("[board-info] FAILED: %s\n", errorName(info.error));
    }
    if (onBoardInfos)
        onBoardInfos(info);
}

void ShadNetClient::handleRecordScoreReply(const std::vector<uint8_t>& payload) {
    RecordScoreResult res;
    if (payload.empty()) {
        if (onRecordScore)
            onRecordScore(res);
        return;
    }
    res.error = static_cast<ErrorType>(payload[0]);
    if (res.error == ErrorType::NoError && payload.size() >= 5)
        memcpy(&res.rank, payload.data() + 1, 4);
    if (res.error == ErrorType::NoError)
        printf("[record-score] OK  rank=%u\n", res.rank);
    else
        printf("[record-score] FAILED: %s\n", errorName(res.error));
    if (onRecordScore)
        onRecordScore(res);
}

void ShadNetClient::handleRecordScoreDataReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    if (err == ErrorType::NoError)
        printf("[record-score-data] OK\n");
    else
        printf("[record-score-data] FAILED: %s\n", errorName(err));
    if (onRecordScoreData)
        onRecordScoreData(err);
}

void ShadNetClient::handleGetScoreDataReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    std::vector<uint8_t> data;
    if (err == ErrorType::NoError && payload.size() >= 5) {
        uint32_t blobLen = 0;
        memcpy(&blobLen, payload.data() + 1, 4);
        data.assign(payload.begin() + 5, payload.begin() + 5 + blobLen);
        printf("[get-score-data] OK  size=%u bytes\n", blobLen);
    } else if (err != ErrorType::NoError) {
        printf("[get-score-data] FAILED: %s\n", errorName(err));
    }
    if (onGetScoreData)
        onGetScoreData(err, data);
}

void ShadNetClient::handleScoreRangeReply(const std::vector<uint8_t>& payload,
                                          std::function<void(const ScoreRangeResult&)>& cb) {
    ScoreRangeResult result;
    if (payload.empty()) {
        if (cb)
            cb(result);
        return;
    }
    result.error = static_cast<ErrorType>(payload[0]);
    if (result.error != ErrorType::NoError) {
        printf("[score-range] FAILED: %s\n", errorName(result.error));
        if (cb)
            cb(result);
        return;
    }

    score::GetScoreResponse pb;
    if (!pb.ParseFromString(extractBlob(payload, 1))) {
        printf("[score-range] Failed to parse GetScoreResponse\n");
        if (cb)
            cb(result);
        return;
    }

    result.lastSortDate = pb.lastsortdate();
    result.totalRecord = pb.totalrecord();

    for (int i = 0; i < pb.rankarray_size(); ++i) {
        const auto& r = pb.rankarray(i);
        ScoreRankEntry e;
        e.npid = r.npid();
        e.onlineName = r.onlinename();
        e.pcId = r.pcid();
        e.rank = r.rank();
        e.score = r.score();
        e.hasGameData = r.hasgamedata();
        e.recordDate = r.recorddate();
        if (i < pb.commentarray_size())
            e.comment = pb.commentarray(i);
        if (i < pb.infoarray_size())
            e.gameInfo =
                std::vector<uint8_t>(pb.infoarray(i).data().begin(), pb.infoarray(i).data().end());
        result.entries.push_back(std::move(e));
    }

    printf("[score-range] total=%u lastSort=%llu\n", result.totalRecord,
           static_cast<unsigned long long>(result.lastSortDate));
    for (const auto& e : result.entries) {
        printf("  #%u  %-16s  score=%-12lld  pcId=%d  gameData=%s\n", e.rank, e.npid.c_str(),
               static_cast<long long>(e.score), e.pcId, e.hasGameData ? "yes" : "no");
        if (!e.comment.empty())
            printf("       comment: %s\n", e.comment.c_str());
    }
    if (result.entries.empty())
        printf("  (no scores)\n");
    if (cb)
        cb(result);
}
