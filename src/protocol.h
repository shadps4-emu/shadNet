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
