#pragma once
#include <cstdint>
#include <vector>
#include "types.h"

namespace shadnet {
struct Packet {
    PacketType type;
    uint16_t command;
    uint32_t size;
    uint64_t packetId;
    std::vector<uint8_t> payload; // everything after the 15-byte header
};
} // namespace shadnet
