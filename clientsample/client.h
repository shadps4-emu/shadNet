// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "connection.h"

namespace shadnetclient {
// Result types

struct FriendEntry {
    std::string npid;
    bool online = false;
};

struct LoginResult {
    ErrorType error = ErrorType::Malformed;
    std::string avatarUrl;
    uint64_t userId = 0;
    std::vector<FriendEntry> friends;
    std::vector<std::string> friendRequestsSent;
    std::vector<std::string> friendRequestsReceived;
    std::vector<std::string> blocked;
};

struct FriendResult {
    CommandType cmd;
    ErrorType error = ErrorType::Malformed;
    std::string targetNpid;
};

// NPScore result types

struct BoardInfo {
    ErrorType error = ErrorType::Malformed;
    uint32_t rankLimit = 0;
    uint32_t updateMode = 0; // 0=NORMAL_UPDATE, 1=FORCE_UPDATE
    uint32_t sortMode = 0;   // 0=DESCENDING, 1=ASCENDING
    uint32_t uploadNumLimit = 0;
    uint64_t uploadSizeLimit = 0;
};

struct ScoreRankEntry {
    std::string npid;
    int32_t pcId = 0;
    uint32_t rank = 0;
    int64_t score = 0;
    bool hasGameData = false;
    uint64_t recordDate = 0;
    int64_t accountId = 0;         // server user_id; matches LoginResult::userId
    std::string comment;           // only when withComment=true
    std::vector<uint8_t> gameInfo; // only when withGameInfo=true
};

struct ScoreRangeResult {
    ErrorType error = ErrorType::Malformed;
    std::vector<ScoreRankEntry> entries;
    uint64_t lastSortDate = 0;
    uint32_t totalRecord = 0;
};

struct RecordScoreResult {
    ErrorType error = ErrorType::Malformed;
    uint32_t rank = 0; // 1-based; rankLimit+1 = didn't make the board
};

// Notification structs

struct NotifyFriendQuery {
    std::string fromNpid;
};
struct NotifyFriendNew {
    std::string npid;
    bool online;
};
struct NotifyFriendLost {
    std::string npid;
};
struct NotifyFriendStatus {
    std::string npid;
    bool online;
    uint64_t timestamp;
};

// ── Matching data types ──────────────────────────────────────────────────────

struct MatchingBinAttr {
    uint32_t attrId = 0;
    std::vector<uint8_t> data;
};

struct MatchingIntAttr {
    uint32_t attrId = 0;
    uint32_t attrValue = 0;
};

struct MatchingRoomGroup {
    uint32_t groupId = 0;
    bool hasPasswd = false;
    bool hasLabel = false;
    std::vector<uint8_t> label;
    uint32_t slots = 0;
    uint32_t numMembers = 0;
};

struct MatchingRoomMemberData {
    std::string npid;
    uint32_t memberId = 0;
    uint32_t teamId = 0;
    bool isOwner = false;
    MatchingRoomGroup group;
    std::vector<MatchingBinAttr> binAttrsInternal;
};

struct MatchingRoomDataInternal {
    uint32_t publicSlots = 0;
    uint32_t privateSlots = 0;
    uint32_t openPublicSlots = 0;
    uint32_t openPrivateSlots = 0;
    uint32_t maxSlot = 0;
    uint32_t serverId = 0;
    uint32_t worldId = 0;
    uint32_t lobbyId = 0;
    uint64_t roomId = 0;
    uint64_t passwdSlotMask = 0;
    uint32_t joinedSlotMask = 0;
    std::vector<MatchingRoomGroup> groups;
    uint32_t flags = 0;
    std::vector<MatchingBinAttr> binAttrsInternal;
};

struct MatchingRoomDataExternal {
    uint32_t maxSlot = 0;
    uint32_t curMembers = 0;
    uint32_t flags = 0;
    uint32_t serverId = 0;
    uint32_t worldId = 0;
    uint64_t lobbyId = 0;
    uint64_t roomId = 0;
    uint64_t passwdSlotMask = 0;
    uint64_t joinedSlotMask = 0;
    uint32_t publicSlots = 0;
    uint32_t privateSlots = 0;
    uint32_t openPublicSlots = 0;
    uint32_t openPrivateSlots = 0;
    std::vector<MatchingRoomGroup> groups;
    std::vector<MatchingIntAttr> externalSearchIntAttrs;
    std::vector<MatchingBinAttr> externalSearchBinAttrs;
    std::vector<MatchingBinAttr> externalBinAttrs;
    std::string ownerNpid;
};

struct CreateJoinRoomResponse {
    MatchingRoomDataInternal roomData;
    std::vector<MatchingRoomMemberData> members;
    uint32_t meMemberId = 0;
    uint32_t ownerMemberId = 0;
};

// ── Matching result types ─────────────────────────────────────────────────────

struct CreateRoomResult {
    ErrorType error = ErrorType::Malformed;
    uint64_t roomId = 0;
    uint32_t serverId = 0;
    uint32_t worldId = 0;
    uint32_t lobbyId = 0;
    uint32_t memberId = 0;
    uint32_t maxSlots = 0;
    uint32_t flags = 0;
    uint32_t curMembers = 0;
    CreateJoinRoomResponse details;
};

struct JoinRoomResult {
    ErrorType error = ErrorType::Malformed;
    uint64_t roomId = 0;
    uint32_t memberId = 0;
    uint32_t maxSlots = 0;
    uint32_t flags = 0;
    uint32_t curMembers = 0;
    CreateJoinRoomResponse details;
};

struct LeaveRoomResult {
    ErrorType error = ErrorType::Malformed;
    uint64_t roomId = 0;
};

struct RoomListResult {
    ErrorType error = ErrorType::Malformed;
    std::vector<MatchingRoomDataExternal> rooms;
};

struct SignalingInfosResult {
    ErrorType error = ErrorType::Malformed;
    std::string targetNpid;
    std::string targetIp;
    uint32_t targetPort = 0;
    uint32_t targetMemberId = 0;
};

struct SetRoomDataResult {
    ErrorType error = ErrorType::Malformed;
    uint64_t roomId = 0;
};

struct KickoutRoomMemberResult {
    ErrorType error = ErrorType::Malformed;
    uint64_t roomId = 0;
};

// ── Matching notification types ───────────────────────────────────────────────

struct NotifyRequestEvent {
    uint32_t ctxId = 0;
    uint32_t serverId = 0;
    uint32_t worldId = 0;
    uint32_t lobbyId = 0;
    uint32_t reqEvent = 0;
    uint32_t reqId = 0;
    uint32_t errorCode = 0;
    uint64_t roomId = 0;
    uint32_t memberId = 0;
    uint32_t maxSlots = 0;
    uint32_t flags = 0;
    bool isOwner = false;
    std::vector<uint8_t> responseBlob;
};

struct NotifyMemberJoined {
    uint64_t roomId = 0;
    uint32_t memberId = 0;
    std::string npid;
    std::string addr;
    uint32_t port = 0;
    std::vector<MatchingBinAttr> binAttrs;
};

struct NotifyMemberLeft {
    uint64_t roomId = 0;
    uint32_t memberId = 0;
    std::string npid;
};

struct NotifySignalingHelper {
    std::string npid;
    uint32_t memberId = 0;
    std::string addr;
    uint32_t port = 0;
};

struct NotifySignalingEvent {
    uint32_t eventType = 0;
    uint64_t roomId = 0;
    uint32_t memberId = 0;
    uint32_t connId = 0;
};

struct NotifyNpSignalingEvent {
    uint32_t event = 0;
    std::string npid;
};

struct NotifyRoomDataInternalUpdated {
    uint64_t roomId = 0;
    uint32_t flags = 0;
    std::vector<MatchingBinAttr> binAttrs;
};

struct NotifyKickedOut {
    uint64_t roomId = 0;
    int32_t statusCode = 0;
    uint32_t guardValue = 0;
};

// ── Matching command params ───────────────────────────────────────────────────

struct MatchingCallbackEntry {
    bool enabled = false;
    uint64_t callbackAddr = 0;
    uint64_t callbackArg = 0;
};

struct RegisterHandlersParams {
    std::string addr;
    uint32_t port = 0;
    uint32_t ctxId = 0;
    uint32_t serviceLabel = 0;
    std::vector<MatchingCallbackEntry> callbacks;
};

struct CreateRoomParams {
    uint32_t reqId = 0;
    uint32_t maxSlots = 8;
    uint32_t teamId = 0;
    uint32_t worldId = 0;
    uint32_t lobbyId = 0;
    uint32_t flags = 0;
    uint32_t groupConfigCount = 0;
    uint32_t allowedUserCount = 0;
    uint32_t blockedUserCount = 0;
    uint32_t internalBinAttrCount = 0;
    std::vector<MatchingIntAttr> externalSearchIntAttrs;
    std::vector<MatchingBinAttr> externalSearchBinAttrs;
    std::vector<MatchingBinAttr> externalBinAttrs;
    std::vector<MatchingBinAttr> memberBinAttrs;
    bool joinGroupLabelPresent = false;
    bool roomPasswordPresent = false;
    uint32_t sigType = 0;
    uint32_t sigFlag = 0;
    uint32_t sigMainMember = 0;
};

struct JoinRoomParams {
    uint64_t roomId = 0;
    uint32_t reqId = 0;
    uint32_t teamId = 0;
    uint32_t joinFlags = 0;
    uint32_t blockedUserCount = 0;
    std::vector<MatchingBinAttr> memberBinAttrs;
    bool roomPasswordPresent = false;
    bool joinGroupLabelPresent = false;
};

struct SetRoomDataInternalParams {
    uint32_t reqId = 0;
    uint64_t roomId = 0;
    uint32_t flagFilter = 0;
    uint32_t flagAttr = 0;
    std::vector<MatchingBinAttr> binAttrs;
    bool hasPasswdMask = false;
    uint64_t passwdSlotMask = 0;
};

struct SetRoomDataExternalParams {
    uint32_t reqId = 0;
    uint64_t roomId = 0;
    std::vector<MatchingIntAttr> searchIntAttrs;
    std::vector<MatchingBinAttr> searchBinAttrs;
    std::vector<MatchingBinAttr> extBinAttrs;
};

struct KickoutRoomMemberParams {
    uint64_t roomId = 0;
    uint32_t reqId = 0;
    uint32_t targetMemberId = 0;
    uint32_t blockKickFlag = 0;
};

// ── Client──────

class ShadNetClient {
public:
    bool connect(const char* host, uint16_t port);
    void disconnect();
    void update();

    // Account commands
    void login(const std::string& npid, const std::string& password, const std::string& token = {});

    void createAccount(const std::string& npid, const std::string& password,
                       const std::string& avatarUrl, const std::string& email,
                       const std::string& secretKey = {});

    // Friend commands
    void addFriend(const std::string& targetNpid);
    void removeFriend(const std::string& targetNpid);
    void addBlock(const std::string& targetNpid);
    void removeBlock(const std::string& targetNpid);

    // NPScore commands
    // comId: 12-char Communication ID, e.g. "NPWR12345_00".

    void getBoardInfos(const std::string& comId, uint32_t boardId);

    void recordScore(const std::string& comId, uint32_t boardId, int32_t pcId, int64_t score,
                     const std::string& comment = {}, const std::vector<uint8_t>& gameInfo = {});

    void recordScoreData(const std::string& comId, uint32_t boardId, int32_t pcId, int64_t score,
                         const std::vector<uint8_t>& data);

    void getScoreData(const std::string& comId, uint32_t boardId, const std::string& npid,
                      int32_t pcId);

    void getScoreRange(const std::string& comId, uint32_t boardId, uint32_t startRank,
                       uint32_t numRanks, bool withComment = false, bool withGameInfo = false);

    struct NpIdPcId {
        std::string npid;
        int32_t pcId = 0;
    };
    void getScoreNpid(const std::string& comId, uint32_t boardId,
                      const std::vector<NpIdPcId>& targets, bool withComment = false,
                      bool withGameInfo = false);

    void getScoreFriends(const std::string& comId, uint32_t boardId, bool includeSelf = true,
                         uint32_t max = 100, bool withComment = false, bool withGameInfo = false);

    struct AccountIdPcId {
        int64_t accountId = 0;
        int32_t pcId = 0;
    };
    void getScoreAccountId(const std::string& comId, uint32_t boardId,
                           const std::vector<AccountIdPcId>& targets, bool withComment = false,
                           bool withGameInfo = false);

    void getScoreGameDataByAccountId(const std::string& comId, uint32_t boardId, int64_t accountId,
                                     int32_t pcId);
    // Matching commands

    void registerHandlers(const RegisterHandlersParams& p);
    void createRoom(const CreateRoomParams& p);
    void joinRoom(const JoinRoomParams& p);
    void leaveRoom(uint64_t roomId, uint32_t reqId = 0);
    void getRoomList();
    void requestSignalingInfos(const std::string& targetNpid);
    void signalingEstablished(const std::string& targetNpid, uint32_t connId);
    void activationConfirm(const std::string& meId, const std::string& initiatorIp,
                           uint32_t ctxTag);
    void setRoomDataInternal(const SetRoomDataInternalParams& p);
    void setRoomDataExternal(const SetRoomDataExternalParams& p);
    void kickoutRoomMember(const KickoutRoomMemberParams& p);

    // Callbacks
    std::function<void(const LoginResult&)> onLoginResult;
    std::function<void(ErrorType)> onCreateResult;
    std::function<void(const FriendResult&)> onFriendResult;

    std::function<void(const BoardInfo&)> onBoardInfos;
    std::function<void(const RecordScoreResult&)> onRecordScore;
    std::function<void(ErrorType)> onRecordScoreData;
    std::function<void(ErrorType, const std::vector<uint8_t>&)> onGetScoreData;
    std::function<void(const ScoreRangeResult&)> onScoreRange;
    std::function<void(const ScoreRangeResult&)> onScoreNpid;
    std::function<void(const ScoreRangeResult&)> onScoreFriends;
    std::function<void(const ScoreRangeResult&)> onScoreAccountId;
    std::function<void(ErrorType, const std::vector<uint8_t>&)> onGetScoreGameDataByAccountId;

    std::function<void(const NotifyFriendQuery&)> onFriendQuery;
    std::function<void(const NotifyFriendNew&)> onFriendNew;
    std::function<void(const NotifyFriendLost&)> onFriendLost;
    std::function<void(const NotifyFriendStatus&)> onFriendStatus;

    // Matching command result callbacks
    std::function<void(ErrorType)> onRegisterHandlers;
    std::function<void(const CreateRoomResult&)> onCreateRoom;
    std::function<void(const JoinRoomResult&)> onJoinRoom;
    std::function<void(const LeaveRoomResult&)> onLeaveRoom;
    std::function<void(const RoomListResult&)> onRoomList;
    std::function<void(const SignalingInfosResult&)> onSignalingInfos;
    std::function<void(ErrorType)> onSignalingEstablished;
    std::function<void(ErrorType)> onActivationConfirm;
    std::function<void(const SetRoomDataResult&)> onSetRoomDataInternal;
    std::function<void(const SetRoomDataResult&)> onSetRoomDataExternal;
    std::function<void(const KickoutRoomMemberResult&)> onKickoutRoomMember;

    // Matching notification callbacks
    std::function<void(const NotifyRequestEvent&)> onRequestEvent;
    std::function<void(const NotifyMemberJoined&)> onMemberJoined;
    std::function<void(const NotifyMemberLeft&)> onMemberLeft;
    std::function<void(const NotifySignalingHelper&)> onSignalingHelper;
    std::function<void(const NotifySignalingEvent&)> onSignalingEvent;
    std::function<void(const NotifyNpSignalingEvent&)> onNpSignalingEvent;
    std::function<void(const NotifyRoomDataInternalUpdated&)> onRoomDataInternalUpdated;
    std::function<void(const NotifyKickedOut&)> onKickedOut;

    const std::string& lastError() const {
        return conn.lastError();
    }

private:
    ShadNetConnection conn;
    std::atomic<uint64_t> packetCounter{1};

    std::vector<uint8_t> buildPacket(CommandType cmd, uint64_t id,
                                     const std::vector<uint8_t>& payload);
    void sendFriendCommand(CommandType cmd, const std::string& targetNpid);

    void handlePacket(const Packet& pkt);
    void handleLoginReply(const std::vector<uint8_t>& payload);
    void handleCreateReply(const std::vector<uint8_t>& payload);
    void handleFriendReply(CommandType cmd, const std::vector<uint8_t>& payload);
    void handleNotification(const Packet& pkt);

    void handleBoardInfosReply(const std::vector<uint8_t>& payload);
    void handleRecordScoreReply(const std::vector<uint8_t>& payload);
    void handleRecordScoreDataReply(const std::vector<uint8_t>& payload);
    void handleGetScoreDataReply(const std::vector<uint8_t>& payload);
    void handleGetScoreDataByAccountIdReply(const std::vector<uint8_t>& payload);
    void handleScoreRangeReply(const std::vector<uint8_t>& payload,
                               std::function<void(const ScoreRangeResult&)>& cb);

    // Matching reply handlers
    void handleRegisterHandlersReply(const std::vector<uint8_t>& payload);
    void handleCreateRoomReply(const std::vector<uint8_t>& payload);
    void handleJoinRoomReply(const std::vector<uint8_t>& payload);
    void handleLeaveRoomReply(const std::vector<uint8_t>& payload);
    void handleRoomListReply(const std::vector<uint8_t>& payload);
    void handleSignalingInfosReply(const std::vector<uint8_t>& payload);
    void handleSimpleMatchingReply(CommandType cmd, const std::vector<uint8_t>& payload);
    void handleSetRoomDataInternalReply(const std::vector<uint8_t>& payload);
    void handleSetRoomDataExternalReply(const std::vector<uint8_t>& payload);
    void handleKickoutRoomMemberReply(const std::vector<uint8_t>& payload);

    std::string m_pendingFriendNpid;
    CommandType m_pendingFriendCmd = CommandType::AddFriend;
};
} // namespace shadnetclient
