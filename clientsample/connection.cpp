// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include "connection.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
// Map POSIX close -> closesocket, and handle WSAEWOULDBLOCK
#define SOCK_CLOSE(s) closesocket(s)
#define SOCK_WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCK_CLOSE(s) ::close(s)
#define SOCK_WOULD_BLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

using namespace shadnet;

static void platformInit() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        done = true;
    }
#endif
}

ShadNetConnection::ShadNetConnection() {
    platformInit();
}

ShadNetConnection::~ShadNetConnection() {
    disconnect();
}

bool ShadNetConnection::connect(const char* host, uint16_t port) {
    disconnect();

    m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock == INVALID_SOCK) {
        m_lastError = "socket() failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        m_lastError = std::string("Invalid address: ") + host;
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
        return false;
    }

    if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        m_lastError = "connect() failed";
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
        return false;
    }

    // The server sends it immediately on connection; verify protocol version.
    uint8_t hdr[HEADER_SIZE];
    if (!recvAll(hdr, static_cast<int>(HEADER_SIZE))) {
        m_lastError = "Timeout waiting for ServerInfo";
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
        return false;
    }

    if (static_cast<PacketType>(hdr[0]) != PacketType::ServerInfo) {
        m_lastError = "Expected ServerInfo packet";
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
        return false;
    }

    uint32_t totalSz = fromLE32(hdr + 3);
    uint32_t payloadSz = totalSz > HEADER_SIZE ? totalSz - HEADER_SIZE : 0;

    std::vector<uint8_t> siPayload(payloadSz);
    if (payloadSz > 0 && !recvAll(siPayload.data(), static_cast<int>(payloadSz))) {
        m_lastError = "Timeout reading ServerInfo payload";
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
        return false;
    }

    if (payloadSz >= 4) {
        uint32_t version = fromLE32(siPayload.data());
        if (version != PROTOCOL_VERSION) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Protocol version mismatch: server=%u client=%u", version,
                     PROTOCOL_VERSION);
            m_lastError = buf;
            SOCK_CLOSE(m_sock);
            m_sock = INVALID_SOCK;
            return false;
        }
    }

    // Switch socket to non-blocking for the update() poll loop
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(m_sock, FIONBIO, &mode);
#else
    int flags = fcntl(m_sock, F_GETFL, 0);
    fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
#endif

    return true;
}

void ShadNetConnection::disconnect() {
    if (m_sock != INVALID_SOCK) {
        SOCK_CLOSE(m_sock);
        m_sock = INVALID_SOCK;
    }
    m_readBuf.clear();
}

bool ShadNetConnection::isConnected() const {
    return m_sock != INVALID_SOCK;
}

bool ShadNetConnection::recvAll(uint8_t* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = static_cast<int>(::recv(m_sock, reinterpret_cast<char*>(buf + got), n - got, 0));
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}

// ── send — blocking send of all bytes ────────────────────────────────────────

bool ShadNetConnection::send(const std::vector<uint8_t>& data) {
    int total = static_cast<int>(data.size());
    int sent = 0;
    while (sent < total) {
        // Use ::send() not ::write() — write() is not available on Windows
        int r = static_cast<int>(
            ::send(m_sock, reinterpret_cast<const char*>(data.data() + sent), total - sent, 0));
        if (r < 0) {
            m_lastError = "send() failed";
            return false;
        }
        sent += r;
    }
    return true;
}

void ShadNetConnection::update() {
    if (m_sock == INVALID_SOCK)
        return;

    uint8_t buf[4096];
    bool serverClosed = false;

    while (true) {
        int r = static_cast<int>(::recv(m_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0));

        if (r > 0) {
            m_readBuf.insert(m_readBuf.end(), buf, buf + r);
        } else if (r == 0) {
            // Server closed its end — do NOT disconnect yet.
            // Break out and parse whatever we already buffered first.
            serverClosed = true;
            break;
        } else {
            if (SOCK_WOULD_BLOCK)
                break;           // no more data right now
            serverClosed = true; // socket error — still drain buffer
            break;
        }
    }

    // Always parse buffered bytes BEFORE tearing down the socket.
    parse();

    if (serverClosed)
        disconnect();
}

// ── parse ─────────────────────────────────────────────────────────────────────

void ShadNetConnection::parse() {
    while (m_readBuf.size() >= HEADER_SIZE) {
        const uint8_t* data = m_readBuf.data();

        uint32_t totalSize = fromLE32(data + 3);

        if (totalSize < HEADER_SIZE) {
            // Corrupt packet — drop everything
            m_readBuf.clear();
            return;
        }

        if (m_readBuf.size() < totalSize)
            return; // wait for more bytes

        Packet pkt;
        pkt.type = static_cast<PacketType>(data[0]);
        pkt.command = fromLE16(data + 1);
        pkt.size = totalSize;
        pkt.packetId = fromLE64(data + 7);
        pkt.payload.assign(data + HEADER_SIZE, data + totalSize);

        m_readBuf.erase(m_readBuf.begin(), m_readBuf.begin() + static_cast<int>(totalSize));

        if (onPacket)
            onPacket(pkt);
    }
}
