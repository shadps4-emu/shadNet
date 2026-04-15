// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "packet.h"

#ifdef _WIN32
#include <winsock2.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle INVALID_SOCK = INVALID_SOCKET;
#else
using SocketHandle = int;
static constexpr SocketHandle INVALID_SOCK = -1;
#endif

namespace shadnetclient {
class ShadNetConnection {
public:
    ShadNetConnection();
    ~ShadNetConnection();

    // Blocking connect + receive ServerInfo handshake.
    // Returns false and sets lastError on failure.
    bool connect(const char* host, uint16_t port);
    void disconnect();
    bool isConnected() const;

    // Blocking send of all bytes.
    bool send(const std::vector<uint8_t>& data);

    // Non-blocking poll — drains the socket and fires onPacket for every
    // complete packet received. Call from your main/update loop.
    void update();

    // Set before calling connect(). Fired for every complete inbound packet.
    std::function<void(const Packet&)> onPacket;

    const std::string& lastError() const {
        return m_lastError;
    }

private:
    SocketHandle m_sock = INVALID_SOCK;
    std::vector<uint8_t> m_readBuf;
    std::string m_lastError;

    // Blocking receive of exactly n bytes (used for ServerInfo handshake).
    bool recvAll(uint8_t* buf, int n);

    // Parse m_readBuf and fire onPacket for each complete packet.
    void parse();
};
} // namespace shadnetclient
