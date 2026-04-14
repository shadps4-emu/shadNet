// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
#include "types.h"

namespace shadnetclient {
struct Packet {
    PacketType type;
    uint16_t command;
    uint32_t size;
    uint64_t packetId;
    std::vector<uint8_t> payload; // everything after the 15-byte header
};
} // namespace shadnetclient
