// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QReadWriteLock>
#include <QString>
#include <QVector>
#include "matching.h"

struct IntAttrSlot {
    bool set = false;
    uint16_t attrId = 0;
    uint32_t value = 0;
};

struct BinAttrSlot {
    bool set = false;
    uint16_t attrId = 0;
    QByteArray data;
};

struct InternalBinAttrSlot {
    bool set = false;
    uint16_t attrId = 0;
    QByteArray data;
    uint64_t updateDate = 0;
    uint16_t updateMemberId = 0;
};

struct MemberBinAttrSlot {
    bool set = false;
    uint16_t attrId = 0;
    QByteArray data;
    uint64_t updateDate = 0;
};

struct RoomGroup {
    uint8_t groupId = 0;
    uint32_t slotNum = 0;
    std::optional<QByteArray> label;
    bool fixedLabel = false;
    bool withPassword = false;
    uint32_t numMembers = 0;
};

struct RoomMember {
    uint16_t memberId = 0;
    int64_t userId = 0;
    QString npid;
    QString onlineName;
    QString avatarUrl;
    QString addr;
    uint16_t port = 0;
    uint32_t platform = 3;
    QVector<QString> blockedUsers;
    QVector<uint64_t> blockedAccountIds;
    uint64_t joinDate = 0;
    uint32_t flagAttr = 0;
    uint8_t teamId = 0;
    uint8_t groupId = 0;
    uint8_t natType = 0;
    MemberBinAttrSlot memberBinAttr;
};

struct MatchingSessionState {
    uint32_t ctxId = 0;
    QString titleId;
    QString matchingKey;
    uint16_t serverId = 1;
    uint16_t worldId = 1;
    uint16_t lobbyId = 0;
    uint64_t roomId = 0;
    uint16_t myMemberId = 0;
    bool isRoomOwner = false;
    uint16_t maxSlots = 0;
    uint32_t roomFlags = 0;
    bool initialized = false;
};

struct Room {
    uint64_t roomId = 0;
    uint16_t serverId = 1;
    uint32_t worldId = 0;
    uint64_t lobbyId = 0;

    uint16_t maxSlot = 0;
    uint16_t publicSlots = 0;
    uint16_t privateSlots = 0;
    uint16_t openPublicSlots = 0;
    uint16_t openPrivateSlots = 0;
    uint64_t passwdSlotMask = 0;

    uint32_t flagAttr = 0;
    std::optional<QByteArray> roomPassword;
    QVector<QString> allowedUsers;
    QVector<QString> blockedUsers;
    QVector<uint64_t> allowedAccountIds;
    QVector<uint64_t> blockedAccountIds;

    uint16_t ownerMemberId = 0;
    QVector<uint16_t> ownerSuccession;

    QVector<RoomGroup> groups;

    IntAttrSlot searchIntAttr[8];
    BinAttrSlot searchBinAttr;
    BinAttrSlot externalBinAttr[2];
    InternalBinAttrSlot internalBinAttr[2];

    uint8_t signalingType = 0;
    uint8_t signalingFlag = 0;
    uint16_t signalingHubMemberId = 0;

    QHash<uint16_t, RoomMember> members;
    uint16_t memberIdCounter = 0;

    RoomMember* findById(uint16_t memberId) {
        auto it = members.find(memberId);
        return it == members.end() ? nullptr : &(*it);
    }

    uint16_t allocMemberId() {
        uint16_t slot = 1;
        for (; slot <= 64; ++slot) {
            bool taken = false;
            for (auto it = members.begin(); it != members.end(); ++it)
                if ((it->memberId >> 4) == slot) {
                    taken = true;
                    break;
                }
            if (!taken)
                break;
        }
        memberIdCounter = static_cast<uint16_t>((memberIdCounter + 1) & 0xF);
        return static_cast<uint16_t>((slot << 4) | memberIdCounter);
    }

    RoomMember* addMember(const RoomMember& proto) {
        RoomMember m = proto;
        m.memberId = allocMemberId();
        members.insert(m.memberId, m);
        return &members[m.memberId];
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

    bool removeMember(uint16_t memberId) {
        auto it = members.find(memberId);
        if (it == members.end())
            return false;
        members.erase(it);
        return true;
    }

    bool isEmpty() const {
        return members.isEmpty();
    }
    bool isFull() const {
        return members.size() >= maxSlot;
    }

    uint64_t joinedSlotMask() const {
        uint64_t mask = 0;
        for (auto it = members.begin(); it != members.end(); ++it) {
            const uint16_t slot = static_cast<uint16_t>(it->memberId >> 4);
            if (slot > 0 && slot <= 64)
                mask |= (1ull << (slot - 1));
        }
        return mask;
    }
};

struct WorldConfig {
    uint32_t worldId = 0;
    uint16_t serverId = 1;
    uint32_t lobbiesNum = 0;
    uint32_t maxLobbyMembersNum = 0;
};

inline uint qHash(const QPair<QString, QString>& key, uint seed = 0) {
    return qHash(key.first, seed) ^ qHash(key.second, seed);
}

inline uint qHash(const QPair<uint32_t, uint32_t>& key, uint seed = 0) {
    return qHash(key.first, seed) ^ qHash(key.second, seed);
}

using ComId = QString;

struct MatchingSharedState {
    mutable QReadWriteLock roomsLock;
    QHash<QPair<ComId, uint64_t>, Room> rooms;
    QHash<QPair<ComId, uint32_t>, QVector<uint64_t>> worldRooms;
    QHash<QPair<ComId, uint64_t>, QVector<uint64_t>> lobbyRooms;
    std::atomic<uint64_t> nextRoomId{1};

    QHash<QString, QVector<WorldConfig>> worldConfigs;
    QHash<QString, QString> titleGroups;

    mutable QReadWriteLock udpLock;
    QHash<QString, QPair<QString, uint16_t>> udpExt;
};
