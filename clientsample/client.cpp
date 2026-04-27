// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include "client.h"
#include "shadnet.pb.h"

using namespace shadnetclient;

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

// Serialise a protobuf message as a u32-LE-prefixed blob.
// This is the standard request payload format for all non-score commands.
static std::vector<uint8_t> makeProtoPayload(const google::protobuf::MessageLite& msg) {
    std::string serialised;
    msg.SerializeToString(&serialised);
    uint32_t len = static_cast<uint32_t>(serialised.size());
    std::vector<uint8_t> out(4);
    memcpy(out.data(), &len, 4); // LE on little-endian hosts
    out.insert(out.end(), serialised.begin(), serialised.end());
    return out;
}

// Read a u32-LE-prefixed blob at payload[pos] and return it ready for ParseFromString.
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

// ── Account commands ──────────────────────────────────────────────────────────

void ShadNetClient::login(const std::string& npid, const std::string& password,
                          const std::string& token) {
    shadnet::LoginRequest req;
    req.set_npid(npid);
    req.set_password(password);
    if (!token.empty())
        req.set_token(token);
    conn.send(buildPacket(CommandType::Login, packetCounter++, makeProtoPayload(req)));
}

void ShadNetClient::createAccount(const std::string& npid, const std::string& password,
                                  const std::string& avatarUrl, const std::string& email,
                                  const std::string& secretKey) {
    shadnet::RegistrationRequest req;
    req.set_npid(npid);
    req.set_password(password);
    if (!avatarUrl.empty())
        req.set_avatar_url(avatarUrl);
    req.set_email(email);
    if (!secretKey.empty())
        req.set_secret_key(secretKey);
    conn.send(buildPacket(CommandType::Create, packetCounter++, makeProtoPayload(req)));
}

// ── Friend / block commands ───────────────────────────────────────────────────

void ShadNetClient::sendFriendCommand(CommandType cmd, const std::string& targetNpid) {
    m_pendingFriendCmd = cmd;
    m_pendingFriendNpid = targetNpid;
    shadnet::FriendCommandRequest req;
    req.set_npid(targetNpid);
    conn.send(buildPacket(cmd, packetCounter++, makeProtoPayload(req)));
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

// ── Score wire helpers ────────────────────────────────────────────────────────

// Build score payload: ComId(12 bytes, zero-padded) + u32-LE blob size + proto bytes.
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

// ── Score send methods ────────────────────────────────────────────────────────

void ShadNetClient::getBoardInfos(const std::string& comId, uint32_t boardId) {
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
    shadnet::RecordScoreRequest req;
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
    shadnet::RecordScoreGameDataRequest req;
    req.set_boardid(boardId);
    req.set_pcid(pcId);
    req.set_score(score);
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
    shadnet::GetScoreGameDataRequest req;
    req.set_boardid(boardId);
    req.set_npid(npid);
    req.set_pcid(pcId);
    conn.send(
        buildPacket(CommandType::GetScoreData, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreRange(const std::string& comId, uint32_t boardId, uint32_t startRank,
                                  uint32_t numRanks, bool withComment, bool withGameInfo) {
    shadnet::GetScoreRangeRequest req;
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
    shadnet::GetScoreNpIdRequest req;
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

void ShadNetClient::getScoreAccountId(const std::string& comId, uint32_t boardId,
                                      const std::vector<AccountIdPcId>& targets, bool withComment,
                                      bool withGameInfo) {
    shadnet::GetScoreAccountIdRequest req;
    req.set_boardid(boardId);
    for (const auto& t : targets) {
        auto* e = req.add_ids();
        e->set_accountid(t.accountId);
        e->set_pcid(t.pcId);
    }
    req.set_withcomment(withComment);
    req.set_withgameinfo(withGameInfo);
    conn.send(
        buildPacket(CommandType::GetScoreAccountId, packetCounter++, makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreGameDataByAccountId(const std::string& comId, uint32_t boardId,
                                                int64_t accountId, int32_t pcId) {
    shadnet::GetScoreGameDataByAccountIdRequest req;
    req.set_boardid(boardId);
    req.set_accountid(accountId);
    req.set_pcid(pcId);
    conn.send(buildPacket(CommandType::GetScoreGameDataByAccId, packetCounter++,
                          makeScorePayload(comId, req)));
}

void ShadNetClient::getScoreFriends(const std::string& comId, uint32_t boardId, bool includeSelf,
                                    uint32_t max, bool withComment, bool withGameInfo) {
    shadnet::GetScoreFriendsRequest req;
    req.set_boardid(boardId);
    req.set_includeself(includeSelf);
    req.set_max(max);
    req.set_withcomment(withComment);
    req.set_withgameinfo(withGameInfo);
    conn.send(
        buildPacket(CommandType::GetScoreFriends, packetCounter++, makeScorePayload(comId, req)));
}

// ── Reply handlers ────────────────────────────────────────────────────────────

void ShadNetClient::handleLoginReply(const std::vector<uint8_t>& payload) {
    LoginResult res;
    if (payload.empty()) {
        res.error = ErrorType::Malformed;
    } else {
        res.error = static_cast<ErrorType>(payload[0]);
        if (res.error == ErrorType::NoError) {
            shadnet::LoginReply pb;
            if (pb.ParseFromString(extractBlob(payload, 1))) {
                res.avatarUrl = pb.avatar_url();
                res.userId = pb.user_id();
                for (const auto& f : pb.friends()) {
                    FriendEntry fe;
                    fe.npid = f.npid();
                    fe.online = f.online();
                    res.friends.push_back(std::move(fe));
                }
                for (const auto& n : pb.friend_requests_sent())
                    res.friendRequestsSent.push_back(n);
                for (const auto& n : pb.friend_requests_received())
                    res.friendRequestsReceived.push_back(n);
                for (const auto& n : pb.blocked())
                    res.blocked.push_back(n);
            } else {
                res.error = ErrorType::Malformed;
            }
        }
    }

    if (res.error == ErrorType::NoError) {
        printf("[login] OK userId=%llu\n", static_cast<unsigned long long>(res.userId));
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
    std::string blob = extractBlob(p, 0);

    switch (static_cast<NotificationType>(pkt.command)) {
    case NotificationType::FriendQuery: {
        shadnet::NotifyFriendQuery pb;
        if (!pb.ParseFromString(blob)) {
            printf("[notify] FriendQuery: parse error\n");
            break;
        }
        NotifyFriendQuery n;
        n.fromNpid = pb.from_npid();
        printf("[notify] FriendQuery from %s\n", n.fromNpid.c_str());
        if (onFriendQuery)
            onFriendQuery(n);
        break;
    }
    case NotificationType::FriendNew: {
        shadnet::NotifyFriendNew pb;
        if (!pb.ParseFromString(blob)) {
            printf("[notify] FriendNew: parse error\n");
            break;
        }
        NotifyFriendNew n;
        n.npid = pb.npid();
        n.online = pb.online();
        printf("[notify] FriendNew %s (%s)\n", n.npid.c_str(), n.online ? "online" : "offline");
        if (onFriendNew)
            onFriendNew(n);
        break;
    }
    case NotificationType::FriendLost: {
        shadnet::NotifyFriendLost pb;
        if (!pb.ParseFromString(blob)) {
            printf("[notify] FriendLost: parse error\n");
            break;
        }
        NotifyFriendLost n;
        n.npid = pb.npid();
        printf("[notify] FriendLost %s\n", n.npid.c_str());
        if (onFriendLost)
            onFriendLost(n);
        break;
    }
    case NotificationType::FriendStatus: {
        shadnet::NotifyFriendStatus pb;
        if (!pb.ParseFromString(blob)) {
            printf("[notify] FriendStatus: parse error\n");
            break;
        }
        NotifyFriendStatus n;
        n.npid = pb.npid();
        n.online = pb.online();
        n.timestamp = pb.timestamp();
        printf("[notify] FriendStatus %s is %s\n", n.npid.c_str(), n.online ? "online" : "offline");
        if (onFriendStatus)
            onFriendStatus(n);
        break;
    }
    default:
        printf("[notify] Unknown type=%u\n", pkt.command);
        break;
    }
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
    case CommandType::GetScoreAccountId:
        handleScoreRangeReply(pkt.payload, onScoreAccountId);
        break;
    case CommandType::GetScoreGameDataByAccId:
        handleGetScoreDataByAccountIdReply(pkt.payload);
        break;
    default:
        printf("[recv] Unhandled reply command=%u\n", pkt.command);
        break;
    }
}

// Score reply handlers

void ShadNetClient::handleBoardInfosReply(const std::vector<uint8_t>& payload) {
    BoardInfo info;
    if (payload.empty()) {
        if (onBoardInfos)
            onBoardInfos(info);
        return;
    }
    info.error = static_cast<ErrorType>(payload[0]);
    if (info.error == ErrorType::NoError) {
        shadnet::BoardInfo pb;
        if (pb.ParseFromString(extractBlob(payload, 1))) {
            info.rankLimit = pb.ranklimit();
            info.updateMode = pb.updatemode();
            info.sortMode = pb.sortmode();
            info.uploadNumLimit = pb.uploadnumlimit();
            info.uploadSizeLimit = pb.uploadsizelimit();
        }
        printf("[board-info] rankLimit=%u updateMode=%u sortMode=%u uploadNum=%u uploadSize=%llu\n",
               info.rankLimit, info.updateMode, info.sortMode, info.uploadNumLimit,
               static_cast<unsigned long long>(info.uploadSizeLimit));
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

void ShadNetClient::handleGetScoreDataByAccountIdReply(const std::vector<uint8_t>& payload) {
    ErrorType err = payload.empty() ? ErrorType::Malformed : static_cast<ErrorType>(payload[0]);
    std::vector<uint8_t> data;
    if (err == ErrorType::NoError && payload.size() >= 5) {
        uint32_t blobLen = 0;
        memcpy(&blobLen, payload.data() + 1, 4);
        data.assign(payload.begin() + 5, payload.begin() + 5 + blobLen);
        printf("[get-score-data-accountid] OK  size=%u bytes\n", blobLen);
    } else if (err != ErrorType::NoError) {
        printf("[get-score-data-accountid] FAILED: %s\n", errorName(err));
    }
    if (onGetScoreGameDataByAccountId)
        onGetScoreGameDataByAccountId(err, data);
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
    shadnet::GetScoreResponse pb;
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
        e.pcId = r.pcid();
        e.rank = r.rank();
        e.score = r.score();
        e.hasGameData = r.hasgamedata();
        e.recordDate = r.recorddate();
        e.accountId = r.accountid();
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
        printf("  #%u  %-16s  acct=%-10lld  score=%-12lld  pcId=%d  gameData=%s\n", e.rank,
               e.npid.c_str(), static_cast<long long>(e.accountId), static_cast<long long>(e.score),
               e.pcId, e.hasGameData ? "yes" : "no");
        if (!e.comment.empty())
            printf("       comment: %s\n", e.comment.c_str());
    }
    if (result.entries.empty())
        printf("  (no scores)\n");
    if (cb)
        cb(result);
}
