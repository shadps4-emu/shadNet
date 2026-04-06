// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "connection.h"

namespace shadnet {
// Result types

struct FriendEntry {
    std::string npid;
    bool online = false;
};

struct LoginResult {
    ErrorType error = ErrorType::Malformed;
    std::string onlineName;
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
    uint32_t uploadSizeLimit = 0;
};

struct ScoreRankEntry {
    std::string npid;
    std::string onlineName;
    int32_t pcId = 0;
    uint32_t rank = 0;
    int64_t score = 0;
    bool hasGameData = false;
    uint64_t recordDate = 0;
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

// ── Client──────

class ShadNetClient {
public:
    bool connect(const char* host, uint16_t port);
    void disconnect();
    void update();

    // Account commands
    void login(const std::string& npid, const std::string& password, const std::string& token = {});

    void createAccount(const std::string& npid, const std::string& password,
                       const std::string& onlineName, const std::string& avatarUrl,
                       const std::string& email);

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

    std::function<void(const NotifyFriendQuery&)> onFriendQuery;
    std::function<void(const NotifyFriendNew&)> onFriendNew;
    std::function<void(const NotifyFriendLost&)> onFriendLost;
    std::function<void(const NotifyFriendStatus&)> onFriendStatus;

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
    void handleScoreRangeReply(const std::vector<uint8_t>& payload,
                               std::function<void(const ScoreRangeResult&)>& cb);

    std::string m_pendingFriendNpid;
    CommandType m_pendingFriendCmd = CommandType::AddFriend;
};
} // namespace shadnet
