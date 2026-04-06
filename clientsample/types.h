// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace shadnet {
constexpr uint32_t HEADER_SIZE = 15;
constexpr uint32_t PROTOCOL_VERSION = 1;

enum class PacketType : uint8_t { Request = 0, Reply = 1, Notify = 2, ServerInfo = 3 };

enum class CommandType : uint16_t {
    Login = 0,
    Terminate = 1,
    Create = 2,
    Delete = 3,
    SendToken = 4,
    SendResetToken = 5,
    ResetPassword = 6,
    ResetState = 7,
    AddFriend    = 8,
    RemoveFriend = 9,
    AddBlock     = 10,
    RemoveBlock  = 11,
    // Matchmaking
    RegisterHandlers      = 12,
    CreateRoom            = 13,
    JoinRoom              = 14,
    LeaveRoom             = 15,
    GetRoomList           = 16,
    RequestSignalingInfos = 17,
    SignalingEstablished  = 18,
    ActivationConfirm     = 19,
    SetRoomDataInternal   = 20,
    SetRoomDataExternal   = 21,
    // (22–29 reserved)
    GetBoardInfos   = 30,
    RecordScore     = 31,
    RecordScoreData = 32,
    GetScoreData    = 33,
    GetScoreRange   = 34,
    GetScoreFriends = 35,
    GetScoreNpid    = 36,
};

enum class NotificationType : uint16_t {
    FriendQuery  = 5,
    FriendNew    = 6,
    FriendLost   = 7,
    FriendStatus = 8,
    // Matchmaking
    RequestEvent            = 9,  // Room request completed (create/join/leave)
    MemberJoined            = 10, // A member joined the room
    MemberLeft              = 11, // A member left the room
    SignalingHelper         = 12, // Peer P2P address exchange
    SignalingEvent          = 13, // NpMatching2-layer signaling (0x5102 ESTABLISHED)
    NpSignalingEvent        = 14, // NpSignaling-layer activation event
    RoomDataInternalUpdated = 15, // Room internal data changed
};

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
    CreationBannedEmail = 12,
    CreationExistingEmail = 13,
    Unauthorized = 23,
    DbFail = 24,
    NotFound = 26,
    Blocked = 27,
    AlreadyFriend = 28,
    ScoreNotBest = 29,
    ScoreInvalid = 30,
    ScoreHasData = 31,
    Unsupported = 33,
};

inline const char* errorName(ErrorType e) {
    switch (e) {
    case ErrorType::NoError:
        return "NoError";
    case ErrorType::Malformed:
        return "Malformed";
    case ErrorType::Invalid:
        return "Invalid";
    case ErrorType::InvalidInput:
        return "InvalidInput";
    case ErrorType::TooSoon:
        return "TooSoon";
    case ErrorType::LoginError:
        return "LoginError";
    case ErrorType::LoginAlreadyLoggedIn:
        return "LoginAlreadyLoggedIn";
    case ErrorType::LoginInvalidUsername:
        return "LoginInvalidUsername";
    case ErrorType::LoginInvalidPassword:
        return "LoginInvalidPassword";
    case ErrorType::LoginInvalidToken:
        return "LoginInvalidToken";
    case ErrorType::CreationError:
        return "CreationError";
    case ErrorType::CreationExistingUsername:
        return "CreationExistingUsername";
    case ErrorType::CreationBannedEmail:
        return "CreationBannedEmail";
    case ErrorType::CreationExistingEmail:
        return "CreationExistingEmail";
    case ErrorType::Unauthorized:
        return "Unauthorized";
    case ErrorType::DbFail:
        return "DbFail";
    case ErrorType::NotFound:
        return "NotFound";
    case ErrorType::Blocked:
        return "Blocked";
    case ErrorType::AlreadyFriend:
        return "AlreadyFriend";
    case ErrorType::ScoreNotBest:
        return "ScoreNotBest";
    case ErrorType::ScoreInvalid:
        return "ScoreInvalid";
    case ErrorType::ScoreHasData:
        return "ScoreHasData";
    case ErrorType::Unsupported:
        return "Unsupported";
    default:
        return "Unknown";
    }
}

inline uint16_t toLE16(uint16_t v) {
    uint16_t r;
    uint8_t* p = reinterpret_cast<uint8_t*>(&r);
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    return r;
}
inline uint32_t toLE32(uint32_t v) {
    uint32_t r;
    uint8_t* p = reinterpret_cast<uint8_t*>(&r);
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
    return r;
}
inline uint64_t toLE64(uint64_t v) {
    uint64_t r;
    uint8_t* p = reinterpret_cast<uint8_t*>(&r);
    for (int i = 0; i < 8; ++i)
        p[i] = (v >> (8 * i)) & 0xFF;
    return r;
}
inline uint16_t fromLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t fromLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t fromLE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
} // namespace shadnet
