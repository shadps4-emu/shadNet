// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include "client_session.h"

ErrorType ClientSession::CmdAddFriend(StreamExtractor& data) {
    QString friendNpid = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    // Resolve target npid → userId.
    auto friendIdOpt = m_db->GetUserId(friendNpid);
    if (!friendIdOpt)
        return ErrorType::NotFound;
    int64_t friendId = *friendIdOpt;

    if (friendId == m_info.userId)
        return ErrorType::InvalidInput;

    constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
    constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);

    auto [res, rel] = m_db->GetRelStatus(m_info.userId, friendId);

    if (res == Database::RelResult::Error)
        return ErrorType::DbFail;

    if (res == Database::RelResult::Ok) {
        // Any block on either side prevents the operation.
        if ((rel.caller & B) || (rel.other & B))
            return ErrorType::Blocked;
        // Already requested or already friends.
        if (rel.caller & F)
            return ErrorType::AlreadyFriend;
    }

    uint8_t newCaller = (res == Database::RelResult::Ok ? rel.caller : 0) | F;
    uint8_t newOther = (res == Database::RelResult::Ok ? rel.other : 0);

    if (!m_db->SetRelStatus(m_info.userId, friendId, newCaller, newOther))
        return ErrorType::DbFail;

    bool mutualFriendship = (newOther & F) != 0;

    if (mutualFriendship) {
        // The target had already sent a request — this AddFriend completes the mutual friendship.

        // Update both in-memory friends maps under a single write lock.
        bool targetOnline = false;
        {
            QWriteLocker lk(&m_shared->clientsLock);
            auto selfIt = m_shared->clients.find(m_info.userId);
            if (selfIt != m_shared->clients.end())
                selfIt->friends.insert(friendId, friendNpid);

            auto friendIt = m_shared->clients.find(friendId);
            if (friendIt != m_shared->clients.end()) {
                friendIt->friends.insert(m_info.userId, m_info.npid);
                targetOnline = true;
            }
        }

        // FriendNew payload: online(u8) + npid\0
        // Notify ourselves.
        {
            QByteArray payload;
            payload.append(static_cast<char>(targetOnline ? 1 : 0));
            appendCStr(payload, friendNpid);
            SendSelfNotification(NotificationType::FriendNew, payload);
        }
        // Notify the new friend.
        {
            QByteArray payload;
            payload.append('\x01'); // we are definitely online since we're sending this
            appendCStr(payload, m_info.npid);
            SendNotification(NotificationType::FriendNew, payload, friendId);
        }

        qInfo() << "Friendship formed:" << m_info.npid << "<->" << friendNpid;
    } else {
        // Target has not yet sent a request — this is a new outgoing request.
        // FriendQuery payload: requester_npid\0
        QByteArray payload;
        appendCStr(payload, m_info.npid);
        SendNotification(NotificationType::FriendQuery, payload, friendId);

        qInfo() << "Friend request sent:" << m_info.npid << "->" << friendNpid;
    }

    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRemoveFriend(StreamExtractor& data) {
    QString friendNpid = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    auto friendIdOpt = m_db->GetUserId(friendNpid);
    if (!friendIdOpt)
        return ErrorType::NotFound;
    int64_t friendId = *friendIdOpt;

    if (friendId == m_info.userId)
        return ErrorType::InvalidInput;

    constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);

    auto [res, rel] = m_db->GetRelStatus(m_info.userId, friendId);
    if (res != Database::RelResult::Ok)
        return ErrorType::NotFound;

    // Require at least one side to have the Friend bit.
    if (!(rel.caller & F) && !(rel.other & F))
        return ErrorType::NotFound;

    uint8_t newCaller = rel.caller & static_cast<uint8_t>(~F);
    uint8_t newOther = rel.other & static_cast<uint8_t>(~F);

    bool ok = (newCaller == 0 && newOther == 0)
                  ? m_db->DeleteRel(m_info.userId, friendId)
                  : m_db->SetRelStatus(m_info.userId, friendId, newCaller, newOther);
    if (!ok)
        return ErrorType::DbFail;

    // Remove from both in-memory friends maps.
    {
        QWriteLocker lk(&m_shared->clientsLock);
        auto selfIt = m_shared->clients.find(m_info.userId);
        if (selfIt != m_shared->clients.end())
            selfIt->friends.remove(friendId);

        auto friendIt = m_shared->clients.find(friendId);
        if (friendIt != m_shared->clients.end())
            friendIt->friends.remove(m_info.userId);
    }

    // FriendLost payload: npid\0 of the person being removed (from the recipient's view)
    {
        QByteArray toSelf;
        appendCStr(toSelf, friendNpid);
        SendSelfNotification(NotificationType::FriendLost, toSelf);

        QByteArray toFriend;
        appendCStr(toFriend, m_info.npid);
        SendNotification(NotificationType::FriendLost, toFriend, friendId);
    }

    qInfo() << "Friend removed:" << m_info.npid << "removed" << friendNpid;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdAddBlock(StreamExtractor& data) {
    QString targetNpid = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    auto targetIdOpt = m_db->GetUserId(targetNpid);
    if (!targetIdOpt)
        return ErrorType::NotFound;
    int64_t targetId = *targetIdOpt;

    if (targetId == m_info.userId)
        return ErrorType::InvalidInput;

    constexpr uint8_t F = static_cast<uint8_t>(FriendStatus::Friend);
    constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);

    auto [res, rel] = m_db->GetRelStatus(m_info.userId, targetId);

    uint8_t curCaller = (res == Database::RelResult::Ok) ? rel.caller : 0;
    uint8_t curOther = (res == Database::RelResult::Ok) ? rel.other : 0;

    bool wasFriend = (curCaller & F) && (curOther & F);

    // Set Blocked, clear Friend on both sides.
    uint8_t newCaller = (curCaller | B) & static_cast<uint8_t>(~F);
    uint8_t newOther = curOther & static_cast<uint8_t>(~F);

    if (!m_db->SetRelStatus(m_info.userId, targetId, newCaller, newOther))
        return ErrorType::DbFail;

    if (wasFriend) {
        // Remove from in-memory maps and notify both sides of the lost friendship.
        {
            QWriteLocker lk(&m_shared->clientsLock);
            auto selfIt = m_shared->clients.find(m_info.userId);
            if (selfIt != m_shared->clients.end())
                selfIt->friends.remove(targetId);

            auto targetIt = m_shared->clients.find(targetId);
            if (targetIt != m_shared->clients.end())
                targetIt->friends.remove(m_info.userId);
        }

        QByteArray toSelf;
        appendCStr(toSelf, targetNpid);
        SendSelfNotification(NotificationType::FriendLost, toSelf);

        QByteArray toTarget;
        appendCStr(toTarget, m_info.npid);
        SendNotification(NotificationType::FriendLost, toTarget, targetId);
    }

    qInfo() << m_info.npid << "blocked" << targetNpid;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdRemoveBlock(StreamExtractor& data) {
    QString targetNpid = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    auto targetIdOpt = m_db->GetUserId(targetNpid);
    if (!targetIdOpt)
        return ErrorType::NotFound;
    int64_t targetId = *targetIdOpt;

    if (targetId == m_info.userId)
        return ErrorType::InvalidInput;

    constexpr uint8_t B = static_cast<uint8_t>(FriendStatus::Blocked);

    auto [res, rel] = m_db->GetRelStatus(m_info.userId, targetId);
    if (res != Database::RelResult::Ok)
        return ErrorType::NotFound;
    if (!(rel.caller & B))
        return ErrorType::NotFound; // nothing to unblock

    uint8_t newCaller = rel.caller & static_cast<uint8_t>(~B);
    uint8_t newOther = rel.other;

    bool ok = (newCaller == 0 && newOther == 0)
                  ? m_db->DeleteRel(m_info.userId, targetId)
                  : m_db->SetRelStatus(m_info.userId, targetId, newCaller, newOther);
    if (!ok)
        return ErrorType::DbFail;

    qInfo() << m_info.npid << "unblocked" << targetNpid;
    return ErrorType::NoError;
}
