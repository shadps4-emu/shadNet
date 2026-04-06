// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>
#include "client_session.h"

// ── Presence dump helpers ─────────────────────────────────────────────────────
// Each friend entry in the login reply carries an online flag followed by a
// fixed-layout presence blob.  Offline/unknown friends get an empty blob.
// Layout: ComId(12) + title\0 + status\0 + comment\0 + data_len(u32) + data
static void appendEmptyPresence(QByteArray& buf) {
    // Empty ComId: 9 null bytes + '_' + '0' + '0'
    static const char emptyComId[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, '_', '0', '0'};
    buf.append(emptyComId, 12);
    buf.append('\0');    // title
    buf.append('\0');    // status
    buf.append('\0');    // comment
    appendU32LE(buf, 0); // data length
}

ErrorType ClientSession::CmdCreate(StreamExtractor& data, QByteArray& reply) {
    Q_UNUSED(reply);
    QString npid = data.getString(false);
    QString password = data.getString(false);
    QString onlineName = data.getString(false);
    QString avatarUrl = data.getString(true);
    QString email = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    if (!IsValidNpid(npid))
        return ErrorType::InvalidInput;

    // Check banned email domain
    int at = email.indexOf('@');
    if (at >= 0) {
        QString domain = email.mid(at + 1).toLower();
        if (m_shared->config->IsBannedDomain(domain))
            return ErrorType::CreationBannedEmailProvider;
    }

    if (avatarUrl.isEmpty()) {
        // Set default avatar URL
        avatarUrl = "https://shadps4.net/avatar/default_01.png";
    }
    auto err = m_db->CreateAccount(npid, password, onlineName, avatarUrl, email);
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

ErrorType ClientSession::CmdLogin(StreamExtractor& data, QByteArray& reply) {
    QString npid = data.getString(false);
    QString password = data.getString(false);
    QString token = data.getString(true);
    if (data.error())
        return ErrorType::Malformed;

    bool checkToken = m_shared->config->IsEmailValidated();
    auto userOpt = m_db->CheckUser(npid, password, token, checkToken);
    if (!userOpt) {
        qWarning() << "Login failed for" << npid;
        return ErrorType::LoginInvalidPassword;
    }
    const UserRecord& user = *userOpt;

    if (user.banned) {
        qWarning() << "Banned user attempted login:" << npid;
        return ErrorType::LoginError;
    }

    // Check not already logged in
    {
        QReadLocker lk(&m_shared->clientsLock);
        if (m_shared->clients.contains(user.userId))
            return ErrorType::LoginAlreadyLoggedIn;
    }

    // Update timestamps
    m_db->UpdateLoginTime(user.userId);

    // Populate client info
    m_info.userId = user.userId;
    m_info.npid = npid;
    m_info.onlineName = user.onlineName;
    m_info.avatarUrl = user.avatarUrl;
    m_info.token = user.token;
    m_info.admin = user.admin;
    m_info.statAgent = user.statAgent;
    m_info.banned = user.banned;
    m_authenticated = true;

    // Load relationships before registering in the clients map so we can
    // check which friends are already online.
    UserRelationships rels = m_db->GetRelationships(user.userId);

    // Build reply: onlineName, avatarUrl, userId, then four relationship lists.
    appendCStr(reply, user.onlineName);
    appendCStr(reply, user.avatarUrl);
    appendU64LE(reply, static_cast<uint64_t>(user.userId));

    // ── Friends list ──────────────────────────────────────────────────────────
    // count(u32)  then for each: npid\0 + online(u8) + presence(19 bytes)
    // Presence is always written — empty blob when offline or no presence set.
    {
        QReadLocker lk(&m_shared->clientsLock);
        appendU32LE(reply, static_cast<uint32_t>(rels.friends.size()));
        for (const auto& [friendId, friendNpid] : rels.friends) {
            appendCStr(reply, friendNpid);
            bool online = m_shared->clients.contains(friendId);
            reply.append(static_cast<char>(online ? 1 : 0));
            appendEmptyPresence(reply); // presence detail not implemented yet
        }
    }

    // ── Pending request lists (npid\0 only, no presence) ─────────────────────
    appendU32LE(reply, static_cast<uint32_t>(rels.friendRequestsSent.size()));
    for (const auto& [id, npid] : rels.friendRequestsSent)
        appendCStr(reply, npid);

    appendU32LE(reply, static_cast<uint32_t>(rels.friendRequestsReceived.size()));
    for (const auto& [id, npid] : rels.friendRequestsReceived)
        appendCStr(reply, npid);

    // ── Blocked list ──────────────────────────────────────────────────────────
    appendU32LE(reply, static_cast<uint32_t>(rels.blocked.size()));
    for (const auto& [id, npid] : rels.blocked)
        appendCStr(reply, npid);

    // Collect online friends' send functions before we release any lock, so
    // we can notify them after registration without a nested lock acquisition.
    QVector<std::pair<std::function<void(QByteArray)>, QString>> onlineFriendSenders;

    // Register in global clients map and populate the in-memory friends map.
    {
        QWriteLocker lk(&m_shared->clientsLock);
        SharedState::ClientEntry entry;
        entry.npid = npid;
        entry.send = [this](QByteArray pkt) {
            QMetaObject::invokeMethod(
                this, [this, pkt]() { SendPacket(pkt); }, Qt::QueuedConnection);
        };
        for (const auto& [friendId, friendNpid] : rels.friends) {
            entry.friends.insert(friendId, friendNpid);
            // If the friend is online, add ourselves to their map too.
            auto it = m_shared->clients.find(friendId);
            if (it != m_shared->clients.end()) {
                it->friends.insert(user.userId, npid);
                onlineFriendSenders.append({it->send, friendNpid});
            }
        }
        m_shared->clients[user.userId] = std::move(entry);
        m_shared->npidToUserId[npid] = user.userId;
    }

    // Notify online friends that we just came online.
    // Payload: online(u8=1) + timestamp(u64 LE ns) + npid\0
    {
        QByteArray payload;
        payload.append('\x01');
        uint64_t ts = static_cast<uint64_t>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) *
                      1'000'000ULL;
        appendU64LE(payload, ts);
        appendCStr(payload, npid);
        QByteArray pkt = ClientSession::BuildNotification(NotificationType::FriendStatus, payload);
        for (const auto& [send, friendNpid] : onlineFriendSenders) {
            Q_UNUSED(friendNpid);
            send(pkt);
        }
    }

    qInfo() << "Authenticated:" << npid;
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdDelete(StreamExtractor& data) {
    QString npid = data.getString(false);
    QString password = data.getString(false);
    if (data.error())
        return ErrorType::Malformed;

    // Verify credentials — no token check needed for deletion.
    auto userOpt = m_db->CheckUser(npid, password, "", false);
    if (!userOpt) {
        qWarning() << "CmdDelete: bad credentials for" << npid;
        return ErrorType::LoginInvalidPassword;
    }

    // Only allow deleting own account unless caller is an admin.
    if (userOpt->userId != m_info.userId && !m_info.admin) {
        qWarning() << "CmdDelete: unauthorized attempt by" << m_info.npid << "to delete" << npid;
        return ErrorType::Unauthorized;
    }

    if (!m_db->DeleteUser(userOpt->userId)) {
        qCritical() << "CmdDelete: DB error deleting" << npid;
        return ErrorType::DbFail;
    }

    qInfo() << "Account deleted:" << npid;

    // If the deleted account is the current session, log it out of the clients map.
    if (userOpt->userId == m_info.userId) {
        QWriteLocker lk(&m_shared->clientsLock);
        m_shared->clients.remove(m_info.userId);
        m_authenticated = false;
    }

    QMetaObject::invokeMethod(m_socket, "disconnectFromHost", Qt::QueuedConnection);
    return ErrorType::NoError;
}
