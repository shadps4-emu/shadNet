// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

// AddFriend
// Request:  u32LE blob size + FriendCommandRequest proto
// Reply:    ErrorType(u8) only

// Build a notification QByteArray containing a serialised proto message.
template <typename T>
static QByteArray buildNotifPayload(const T& msg) {
    QByteArray buf;
    appendProto(buf, msg);
    return buf;
}

// WebApi push-event dataTypes the system "npweblis" listener subscribes to (empty
// service name, exact dataType match). Emitted alongside the binary friend/block
// notifications so libSceNpWebApi consumers re-fetch via the friendList/blockingUsers
// routes. See ClientSession::PushWebApiEvent.
static const QString kWebApiFriendDataType = QStringLiteral("np:service:friendlist:friend");

// Build the friendlist:friend extended-data pairs
static QList<QPair<QString, QString>> FriendExtd(const QString& friendNpid, int64_t friendAccountId,
                                                 const char* event) {
    const QString ev = QString::fromLatin1(event);
    QJsonObject trigger;
    trigger.insert(QStringLiteral("friend"), friendNpid);
    trigger.insert(QStringLiteral("event"), ev);
    QJsonObject addl;
    addl.insert(QStringLiteral("friendAccountId"), QString::number(friendAccountId));
    addl.insert(QStringLiteral("event"), ev);
    return {
        {QStringLiteral("trigger"),
         QString::fromUtf8(QJsonDocument(trigger).toJson(QJsonDocument::Compact))},
        {QStringLiteral("additionalTrigger"),
         QString::fromUtf8(QJsonDocument(addl).toJson(QJsonDocument::Compact))},
    };
}
static const QString kWebApiBlockDataType = QStringLiteral("np:service:blocklist");

ErrorType ClientSession::CmdAddFriend(StreamExtractor& data) {
    shadnet::FriendCommandRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString friendNpid = QString::fromStdString(req.npid());

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
        if ((rel.caller & B) || (rel.other & B))
            return ErrorType::Blocked;
        if (rel.caller & F)
            return ErrorType::AlreadyFriend;
    }

    uint8_t newCaller = (res == Database::RelResult::Ok ? rel.caller : 0) | F;
    uint8_t newOther = (res == Database::RelResult::Ok ? rel.other : 0);

    if (!m_db->SetRelStatus(m_info.userId, friendId, newCaller, newOther))
        return ErrorType::DbFail;

    bool mutualFriendship = (newOther & F) != 0;

    if (mutualFriendship) {
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

        // FriendNew to ourselves: tell us about the new friend and whether they're online.
        {
            shadnet::NotifyFriendNew n;
            n.set_npid(friendNpid.toStdString());
            n.set_online(targetOnline);
            SendSelfNotification(NotificationType::FriendNew, buildNotifPayload(n));
        }
        // FriendNew to new friend: tell them about us (we are definitely online).
        {
            shadnet::NotifyFriendNew n;
            n.set_npid(m_info.npid.toStdString());
            n.set_online(true);
            SendNotification(NotificationType::FriendNew, buildNotifPayload(n), friendId);
        }

        // WebApi: both friend lists changed -> tell each side's push listener to refresh.
        PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), m_info.npid,
                        m_info.userId, FriendExtd(friendNpid, friendId, "add"));
        PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), friendNpid,
                        friendId, FriendExtd(m_info.npid, m_info.userId, "add"));

        qInfo() << "Friendship formed:" << m_info.npid << "<->" << friendNpid;
    } else {
        // Outgoing friend request notify the target.
        shadnet::NotifyFriendQuery q;
        q.set_from_npid(m_info.npid.toStdString());
        SendNotification(NotificationType::FriendQuery, buildNotifPayload(q), friendId);

        // WebApi: the requestee's friend data changed (incoming request).
        PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), friendNpid,
                        friendId);

        qInfo() << "Friend request sent:" << m_info.npid << "->" << friendNpid;
    }

    return ErrorType::NoError;
}

// RemoveFriend

ErrorType ClientSession::CmdRemoveFriend(StreamExtractor& data) {
    shadnet::FriendCommandRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString friendNpid = QString::fromStdString(req.npid());

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
    if (!(rel.caller & F) && !(rel.other & F))
        return ErrorType::NotFound;

    uint8_t newCaller = rel.caller & static_cast<uint8_t>(~F);
    uint8_t newOther = rel.other & static_cast<uint8_t>(~F);

    bool ok = (newCaller == 0 && newOther == 0)
                  ? m_db->DeleteRel(m_info.userId, friendId)
                  : m_db->SetRelStatus(m_info.userId, friendId, newCaller, newOther);
    if (!ok)
        return ErrorType::DbFail;

    {
        QWriteLocker lk(&m_shared->clientsLock);
        auto selfIt = m_shared->clients.find(m_info.userId);
        if (selfIt != m_shared->clients.end())
            selfIt->friends.remove(friendId);

        auto friendIt = m_shared->clients.find(friendId);
        if (friendIt != m_shared->clients.end())
            friendIt->friends.remove(m_info.userId);
    }

    // FriendLost to both sides.
    {
        shadnet::NotifyFriendLost toSelf;
        toSelf.set_npid(friendNpid.toStdString());
        SendSelfNotification(NotificationType::FriendLost, buildNotifPayload(toSelf));

        shadnet::NotifyFriendLost toFriend;
        toFriend.set_npid(m_info.npid.toStdString());
        SendNotification(NotificationType::FriendLost, buildNotifPayload(toFriend), friendId);
    }

    // WebApi: both friend lists changed.
    PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), m_info.npid,
                    m_info.userId, FriendExtd(friendNpid, friendId, "remove"));
    PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), friendNpid,
                    friendId, FriendExtd(m_info.npid, m_info.userId, "remove"));

    qInfo() << "Friend removed:" << m_info.npid << "removed" << friendNpid;
    return ErrorType::NoError;
}

// AddBlock

ErrorType ClientSession::CmdAddBlock(StreamExtractor& data) {
    shadnet::FriendCommandRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString targetNpid = QString::fromStdString(req.npid());

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

    uint8_t newCaller = (curCaller | B) & static_cast<uint8_t>(~F);
    uint8_t newOther = curOther & static_cast<uint8_t>(~F);

    if (!m_db->SetRelStatus(m_info.userId, targetId, newCaller, newOther))
        return ErrorType::DbFail;

    if (wasFriend) {
        {
            QWriteLocker lk(&m_shared->clientsLock);
            auto selfIt = m_shared->clients.find(m_info.userId);
            if (selfIt != m_shared->clients.end())
                selfIt->friends.remove(targetId);

            auto targetIt = m_shared->clients.find(targetId);
            if (targetIt != m_shared->clients.end())
                targetIt->friends.remove(m_info.userId);
        }

        shadnet::NotifyFriendLost toSelf;
        toSelf.set_npid(targetNpid.toStdString());
        SendSelfNotification(NotificationType::FriendLost, buildNotifPayload(toSelf));

        shadnet::NotifyFriendLost toTarget;
        toTarget.set_npid(m_info.npid.toStdString());
        SendNotification(NotificationType::FriendLost, buildNotifPayload(toTarget), targetId);

        // WebApi: blocking also removed the friendship on both sides.
        PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), m_info.npid,
                        m_info.userId, FriendExtd(targetNpid, targetId, "remove"));
        PushWebApiEvent(QString(), 0, kWebApiFriendDataType, QByteArray(), QString(), targetNpid,
                        targetId, FriendExtd(m_info.npid, m_info.userId, "remove"));
    }

    // WebApi: our block list changed.
    PushWebApiEvent(QString(), 0, kWebApiBlockDataType, QByteArray(), QString(), m_info.npid,
                    m_info.userId);

    qInfo() << m_info.npid << "blocked" << targetNpid;
    return ErrorType::NoError;
}

// RemoveBlock

ErrorType ClientSession::CmdRemoveBlock(StreamExtractor& data) {
    shadnet::FriendCommandRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString targetNpid = QString::fromStdString(req.npid());

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
        return ErrorType::NotFound;

    uint8_t newCaller = rel.caller & static_cast<uint8_t>(~B);
    uint8_t newOther = rel.other;

    bool ok = (newCaller == 0 && newOther == 0)
                  ? m_db->DeleteRel(m_info.userId, targetId)
                  : m_db->SetRelStatus(m_info.userId, targetId, newCaller, newOther);
    if (!ok)
        return ErrorType::DbFail;

    // WebApi: our block list changed.
    PushWebApiEvent(QString(), 0, kWebApiBlockDataType, QByteArray(), QString(), m_info.npid,
                    m_info.userId);

    qInfo() << m_info.npid << "unblocked" << targetNpid;
    return ErrorType::NoError;
}
