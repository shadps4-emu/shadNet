// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>
#include "client_session.h"
#include "proto_utils.h"
#include "shadnet.pb.h"

// Create
// Request:  u32LE blob size + RegistrationRequest proto
// Reply:    ErrorType(u8) only — no body on success

ErrorType ClientSession::CmdCreate(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(reply);

    shadnet::RegistrationRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString npid = QString::fromStdString(req.npid());
    QString password = QString::fromStdString(req.password());
    QString avatarUrl = QString::fromStdString(req.avatar_url());
    QString email = QString::fromStdString(req.email());
    QString secretKey = QString::fromStdString(req.secret_key());

    // Check registration secret key before anything else.
    if (!m_shared->config->IsRegistrationAllowed(secretKey))
        return ErrorType::Unauthorized;

    if (!IsValidNpid(npid))
        return ErrorType::InvalidInput;

    if (password.isEmpty() || email.isEmpty())
        return ErrorType::InvalidInput;

    // Check banned email domain.
    int at = email.indexOf('@');
    if (at >= 0) {
        QString domain = email.mid(at + 1).toLower();
        if (m_shared->config->IsBannedDomain(domain))
            return ErrorType::CreationBannedEmailProvider;
    }

    if (avatarUrl.isEmpty())
        avatarUrl = "https://shadps4.net/shadnet/avatars/default_01.png";

    // npid is used as display name by the server.
    auto err = m_db->CreateAccount(npid, password, avatarUrl, email);
    if (err) {
        switch (*err) {
        case DbError::ExistingUsername:
            return ErrorType::CreationExistingUsername;
        case DbError::ExistingEmail:
            return ErrorType::CreationExistingEmail;
        default:
            return ErrorType::CreationError;
        }
    }

    qInfo() << "Account created:" << npid;
    return ErrorType::NoError;
}

// Login
// Request:  u32LE blob size + LoginRequest proto
// Reply:    ErrorType(u8) + u32LE blob size + LoginReply proto

ErrorType ClientSession::CmdLogin(StreamExtractor& data, QByteArray& reply) {
    shadnet::LoginRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    QString npid = QString::fromStdString(req.npid());
    QString token = QString::fromStdString(req.token());

    bool checkToken = m_shared->config->IsEmailValidated();
    auto userOpt = m_db->CheckUser(npid, QString::fromStdString(req.password()), token, checkToken);
    if (!userOpt) {
        qWarning() << "Login failed for" << npid;
        return ErrorType::LoginInvalidPassword;
    }
    const UserRecord& user = *userOpt;

    if (user.banned) {
        qWarning() << "Banned user attempted login:" << npid;
        return ErrorType::LoginError;
    }

    {
        QReadLocker lk(&m_shared->clientsLock);
        if (m_shared->clients.contains(user.userId))
            return ErrorType::LoginAlreadyLoggedIn;
    }

    m_db->UpdateLoginTime(user.userId);

    m_info.userId = user.userId;
    m_info.npid = npid;
    m_info.avatarUrl = user.avatarUrl;
    m_info.token = user.token;
    m_info.admin = user.admin;
    m_info.statAgent = user.statAgent;
    m_info.banned = user.banned;
    m_authenticated = true;

    UserRelationships rels = m_db->GetRelationships(user.userId);

    // Build LoginReply
    shadnet::LoginReply pb;
    pb.set_avatar_url(user.avatarUrl.toStdString());
    pb.set_user_id(static_cast<uint64_t>(user.userId));

    {
        QReadLocker lk(&m_shared->clientsLock);
        for (const auto& [friendId, friendNpid] : rels.friends) {
            auto* fe = pb.add_friends();
            fe->set_npid(friendNpid.toStdString());
            fe->set_online(m_shared->clients.contains(friendId));
            // presence field intentionally left empty
        }
    }

    for (const auto& [id, sentNpid] : rels.friendRequestsSent)
        pb.add_friend_requests_sent(sentNpid.toStdString());

    for (const auto& [id, recvNpid] : rels.friendRequestsReceived)
        pb.add_friend_requests_received(recvNpid.toStdString());

    for (const auto& [id, blockedNpid] : rels.blocked)
        pb.add_blocked(blockedNpid.toStdString());

    appendProto(reply, pb);

    // Register session and notify online friends
    QVector<std::pair<std::function<void(QByteArray)>, QString>> onlineFriendSenders;
    {
        QWriteLocker lk(&m_shared->clientsLock);
        SharedState::ClientEntry entry;
        entry.npid = npid;
        entry.send = [this](QByteArray pkt) {
            QMetaObject::invokeMethod(
                this, [this, pkt]() { SendPacket(pkt); }, Qt::QueuedConnection);
        };
        entry.resetMatchingRoomState = [this](uint64_t roomId) {
            QMetaObject::invokeMethod(
                this, [this, roomId]() { ResetMatchingRoomState(roomId); }, Qt::QueuedConnection);
        };
        for (const auto& [friendId, friendNpid] : rels.friends) {
            entry.friends.insert(friendId, friendNpid);
            auto it = m_shared->clients.find(friendId);
            if (it != m_shared->clients.end()) {
                it->friends.insert(user.userId, npid);
                onlineFriendSenders.append({it->send, friendNpid});
            }
        }
        m_shared->clients[user.userId] = std::move(entry);
        m_shared->npidToUserId[npid] = user.userId;
    }

    // Notify online friends: FriendStatus notification (we just came online).
    if (!onlineFriendSenders.isEmpty()) {
        shadnet::NotifyFriendStatus ns;
        ns.set_npid(npid.toStdString());
        ns.set_online(true);
        ns.set_timestamp(
            static_cast<uint64_t>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) *
            1'000'000ULL);

        QByteArray notifPayload;
        appendProto(notifPayload, ns);
        QByteArray pkt =
            ClientSession::BuildNotification(NotificationType::FriendStatus, notifPayload);
        for (const auto& [send, friendNpid] : onlineFriendSenders) {
            Q_UNUSED(friendNpid);
            send(pkt);
        }
    }

    qInfo() << "Authenticated:" << npid;
    return ErrorType::NoError;
}

// Delete

ErrorType ClientSession::CmdDelete(StreamExtractor& data) {
    QString npid = data.getString(false);
    QString password = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    auto userOpt = m_db->CheckUser(npid, password, "", false);
    if (!userOpt) {
        qWarning() << "CmdDelete: bad credentials for" << npid;
        return ErrorType::LoginInvalidPassword;
    }

    if (userOpt->userId != m_info.userId && !m_info.admin) {
        qWarning() << "CmdDelete: unauthorized attempt by" << m_info.npid << "to delete" << npid;
        return ErrorType::Unauthorized;
    }

    if (!m_db->DeleteUser(userOpt->userId)) {
        qCritical() << "CmdDelete: DB error deleting" << npid;
        return ErrorType::DbFail;
    }

    qInfo() << "Account deleted:" << npid;

    if (userOpt->userId == m_info.userId) {
        QWriteLocker lk(&m_shared->clientsLock);
        m_shared->clients.remove(m_info.userId);
        m_authenticated = false;
    }

    QMetaObject::invokeMethod(m_socket, "disconnectFromHost", Qt::QueuedConnection);
    return ErrorType::NoError;
}
