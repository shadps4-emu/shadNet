// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <QString>

static constexpr uint32_t HEADER_SIZE = 15;
static constexpr uint32_t MAX_PACKET_SIZE = 0x800000; // 8 MiB
static constexpr uint32_t PROTOCOL_VERSION = 1;

enum class PacketType : uint8_t {
    Request = 0,
    Reply = 1,
    Notification = 2,
    ServerInfo = 3,
};

// Command IDs for Request packets
enum class CommandType : uint16_t {
    Login = 0,
    Terminate = 1,
    Create = 2,
    Delete = 3,
    SendToken = 4,
    SendResetToken = 5,
    ResetPassword = 6,
    ResetState = 7,
    AddFriend = 8,
    RemoveFriend = 9,
    AddBlock = 10,
    RemoveBlock = 11,
    // Matchmaking
    RegisterHandlers = 12,
    CreateRoom = 13,
    JoinRoom = 14,
    LeaveRoom = 15,
    GetRoomList = 16,
    RequestSignalingInfos = 17,
    SignalingEstablished = 18,
    ActivationConfirm = 19,
    SetRoomDataInternal = 20,
    SetRoomDataExternal = 21,
    // TODO 22-29
    GetBoardInfos = 30,
    RecordScore = 31,
    RecordScoreData = 32,
    GetScoreData = 33,
    GetScoreRange = 34,
    GetScoreFriends = 35,
    GetScoreNpid = 36,

};

// Notification type IDs (u16 LE in Notification packet header).
// These are pushed by the server with no corresponding reply.
enum class NotificationType : uint16_t {
    // (0-4: room notifications, not used in auth-only mode)
    FriendQuery = 5,  // Someone sent you a friend request
    FriendNew = 6,    // Mutual friendship formed (request accepted)
    FriendLost = 7,   // Someone removed you from their friend list
    FriendStatus = 8, // A friend came online or went offline
    // Matchmaking
    RequestEvent = 9,             // Room request completed (create/join/leave)
    MemberJoined = 10,            // A member joined the room
    MemberLeft = 11,              // A member left the room
    SignalingHelper = 12,         // Peer P2P address exchange
    SignalingEvent = 13,          // NpMatching2-layer signaling event (0x5102 ESTABLISHED)
    NpSignalingEvent = 14,        // NpSignaling-layer event (activation confirmed)
    RoomDataInternalUpdated = 15, // Room internal data changed (broadcast to other members)
};

// Error codes for Reply packets
enum class ErrorType : uint8_t {
    NoError = 0,
    Malformed = 1,
    Invalid = 2,
    InvalidInput = 3,
    TooSoon = 4,
    LoginError = 5,
    LoginAlreadyLoggedIn = 6,
    LoginInvalidUsername = 7,
    LoginInvalidPassword = 8,
    LoginInvalidToken = 9,
    CreationError = 10,
    CreationExistingUsername = 11,
    CreationBannedEmailProvider = 12,
    CreationExistingEmail = 13,
    RoomMissing = 14,
    RoomAlreadyJoined = 15,
    RoomFull = 16,
    RoomPasswordMismatch = 17,
    RoomPasswordMissing = 18,
    RoomGroupNoJoinLabel = 19,
    RoomGroupFull = 20,
    RoomGroupJoinLabelNotFound = 21,
    RoomGroupMaxSlotMismatch = 22,
    Unauthorized = 23,
    DbFail = 24,
    EmailFail = 25,
    NotFound = 26,
    Blocked = 27,
    AlreadyFriend = 28,
    ScoreNotBest = 29,
    ScoreInvalid = 30,
    ScoreHasData = 31,
    CondFail = 32,
    Unsupported = 33,
};