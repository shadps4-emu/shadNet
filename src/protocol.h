#pragma once
#include <cstdint>
#include <array>
#include <QString>

static constexpr uint32_t HEADER_SIZE = 15;
static constexpr uint32_t MAX_PACKET_SIZE = 0x800000; // 8 MiB
static constexpr uint32_t PROTOCOL_VERSION = 1;//increase after major changes

enum class PacketType : uint8_t {
	Request = 0,
	Reply = 1,
	Notification = 2,
	ServerInfo = 3,
};

//Command IDs for Request packets
enum class CommandType : uint16_t {
	Login = 0,
	Terminate = 1,
	Create = 2,
};

//Error codes for Reply packets
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