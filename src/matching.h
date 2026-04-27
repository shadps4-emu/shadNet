// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

// ── Handler type indices (array index and bitmask position) ──────────────────

namespace HandlerType {
constexpr uint8_t Context = 0;
constexpr uint8_t Request = 1;
constexpr uint8_t Signaling = 2;
constexpr uint8_t RoomEvent = 3;
constexpr uint8_t LobbyEvent = 4;
constexpr uint8_t RoomMessage = 5;
constexpr uint8_t LobbyMessage = 6;
constexpr uint8_t Count = 7;
} // namespace HandlerType

// ── Room internal binary attribute storage ───────────────────────────────────

struct RoomBinAttrEntry {
    uint16_t attrId = 0;
    QByteArray data;
};

// ── Per-member data within a room ────────────────────────────────────────────

struct RoomMember {
    uint16_t memberId = 0;
    QString npid;
    QString addr;                               // P2P IP
    uint16_t port = 0;                          // P2P UDP port
    QVector<RoomBinAttrEntry> binAttrsInternal; // per-member bin attrs
};

// ── Callback registration for one handler type ──────────────────────────────

struct CallbackRegistration {
    bool enabled = false;
    uint64_t callbackAddr = 0;
    uint64_t callbackArg = 0;
};

// ── Per-session matchmaking state (owned by ClientSession, not shared) ──────

struct MatchingSessionState {
    QString addr;
    uint16_t port = 0;
    uint32_t ctxId = 0;
    uint32_t serviceLabel = 0;
    uint16_t serverId = 1;
    uint16_t worldId = 1;
    uint16_t lobbyId = 0;
    uint64_t roomId = 0;
    uint16_t myMemberId = 0;
    bool isRoomOwner = false;
    uint16_t maxSlots = 0;
    uint32_t roomFlags = 0;
    bool initialized = false;

    CallbackRegistration callbacks[HandlerType::Count] = {};
    uint8_t enabledHandlersMask = 0;

    bool hasHandler(uint8_t idx) const {
        return (enabledHandlersMask & (1 << idx)) != 0;
    }
};

// ── Room (lives inside MatchingSharedState, protected by roomsLock) ──────────

struct Room {
    uint64_t roomId = 0;
    uint16_t maxSlots = 0;
    QString ownerNpid;
    uint16_t serverId = 1;
    uint16_t worldId = 1;
    uint16_t lobbyId = 0;
    uint32_t flags = 0;

    // Room settings from CreateRoom
    uint16_t teamId = 0;
    uint16_t groupConfigCount = 0;
    uint16_t allowedUserCount = 0;
    uint16_t blockedUserCount = 0;
    uint16_t internalBinAttrCount = 0;
    uint16_t externalSearchIntAttrCount = 0;
    uint16_t externalSearchBinAttrCount = 0;
    uint16_t externalBinAttrCount = 0;
    uint16_t memberInternalBinAttrCount = 0;
    uint64_t passwdSlotMask = 0;
    bool joinGroupLabelPresent = false;
    bool roomPasswordPresent = false;
    uint8_t signalingType = 0;
    uint8_t signalingFlag = 0;
    uint16_t signalingMainMember = 0;

    QHash<uint16_t, RoomMember> members; // memberId -> RoomMember
    uint16_t nextMemberId = 1;

    // Internal binary attributes (set via SetRoomDataInternal)
    QVector<RoomBinAttrEntry> binAttrsInternal;

    // External search/bin attributes (set via SetRoomDataExternal)
    struct IntAttrEntry {
        uint16_t attrId = 0;
        uint32_t attrValue = 0;
    };
    QVector<IntAttrEntry> externalSearchIntAttrs;
    QVector<RoomBinAttrEntry> externalSearchBinAttrs;
    QVector<RoomBinAttrEntry> externalBinAttrs;

    RoomMember* addMember(const QString& npid, const QString& addr, uint16_t port) {
        uint16_t mid = nextMemberId++;
        RoomMember m;
        m.memberId = mid;
        m.npid = npid;
        m.addr = addr;
        m.port = port;
        members.insert(mid, m);
        return &members[mid];
    }

    bool removeMember(uint16_t memberId) {
        auto it = members.find(memberId);
        if (it == members.end())
            return false;
        members.erase(it);
        return true;
    }

    const RoomMember* findByNpid(const QString& npid) const {
        for (auto it = members.begin(); it != members.end(); ++it)
            if (it->npid == npid)
                return &(*it);
        return nullptr;
    }

    RoomMember* findByNpidMut(const QString& npid) {
        for (auto it = members.begin(); it != members.end(); ++it)
            if (it->npid == npid)
                return &(*it);
        return nullptr;
    }

    bool isEmpty() const {
        return members.isEmpty();
    }
    bool isFull() const {
        return members.size() >= maxSlots;
    }

    uint32_t joinedSlotMask() const {
        uint32_t mask = 0;
        for (auto it = members.begin(); it != members.end(); ++it)
            if (it->memberId > 0 && it->memberId <= 32)
                mask |= (1u << (it->memberId - 1));
        return mask;
    }
};

// ── QPair hash support for signaling maps ────────────────────────────────────

inline uint qHash(const QPair<QString, QString>& key, uint seed = 0) {
    return qHash(key.first, seed) ^ qHash(key.second, seed);
}

inline uint qHash(const QPair<uint32_t, uint32_t>& key, uint seed = 0) {
    return qHash(key.first, seed) ^ qHash(key.second, seed);
}

// ── Shared matchmaking state (embedded in SharedState) ──────────────────────

struct MatchingSharedState {
    // All rooms. Protected by roomsLock.
    mutable QReadWriteLock roomsLock;
    QHash<uint64_t, Room> rooms;
    std::atomic<uint64_t> nextRoomId{1};

    // External UDP endpoints observed by STUN. Protected by udpLock.
    mutable QReadWriteLock udpLock;
    QHash<QString, QPair<QString, uint16_t>> udpExt; // npid -> (ip, port)

    // Signaling pairs for MutualActivated. Protected by signalingLock.
    mutable QReadWriteLock signalingLock;
    QHash<QPair<QString, QString>, QPair<QString, uint16_t>> signalingPairs;

    // Activation intents. Protected by activationLock.
    mutable QReadWriteLock activationLock;
    // Key: (initiator_ip_u32, ctx_tag), Value: (initiator_npid, peer_npid)
    QHash<QPair<uint32_t, uint32_t>, QPair<QString, QString>> activationIntents;
};
