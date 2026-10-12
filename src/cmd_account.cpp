// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <tuple>
#include <QDateTime>
#include <QDebug>
#include <QTimer>
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
        avatarUrl = "https://shadps4.net/shad_net/assets/avatar/default_01.png";

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

    // Recently rejected for its client version: refuse before signing in, so a client that
    // reconnects on its own never shows up online. Clients retry with a growing delay.
    if (const qint64 left = m_shared->VersionCooldownRemainingMs(user.userId); left > 0) {
        qInfo().noquote() << "Login refused for" << user.username
                          << ": client version was rejected," << (left + 999) / 1000
                          << "s of cooldown left";
        return ErrorType::LoginError;
    }

    {
        QReadLocker lk(&m_shared->clientsLock);
        if (m_shared->clients.contains(user.userId) ||
            m_shared->pendingVersionLogins.contains(user.userId))
            return ErrorType::LoginAlreadyLoggedIn;
    }

    m_db->UpdateLoginTime(user.userId);

    m_info.userId = user.userId;
    m_info.npid = user.username;
    m_info.avatarUrl = user.avatarUrl;
    m_info.token = user.token;
    m_info.admin = user.admin;
    m_info.statAgent = user.statAgent;
    m_info.banned = user.banned;
    m_authenticated = true;

    m_matching.titleId = QString::fromStdString(req.title_id());
    {
        QReadLocker lk(&m_shared->matching.roomsLock);
        m_matching.matchingKey =
            m_shared->matching.titleGroups.value(m_matching.titleId, m_matching.titleId);
    }

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
            // Appear-Offline friends are reported offline.
            const auto fit = m_shared->clients.constFind(friendId);
            fe->set_online(fit != m_shared->clients.constEnd() && !fit->appearOffline);
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

    PendingOnline pending;
    pending.appearOffline = req.appear_offline();
    pending.titleId = QString::fromStdString(req.title_id());
    pending.titleName = QString::fromStdString(req.title_name());
    pending.matching2Access = m_db->GetMatching2Access(user.userId);
    m_pendingOnline = pending;

    // Clients report their version right after signing in (SetClientVersion). With version
    // rules on, the session isn't shown online (friends, presence, online lists, usage) until
    // the version is accepted
    const int versionTimeout = (m_shared->clientVersionRules && m_shared->config)
                                   ? m_shared->config->GetClientVersionTimeoutSeconds()
                                   : 0;
    if (versionTimeout > 0 && !user.statAgent) {
        {
            QWriteLocker lk(&m_shared->clientsLock);
            m_shared->pendingVersionLogins.insert(user.userId);
        }
        QTimer::singleShot(versionTimeout * 1000, this, [this]() {
            if (!m_versionReported)
                CheckClientVersion(QString(), /*reported=*/false);
        });
        qInfo() << "Authenticated:" << npid << "(online once its client version is accepted)";
    } else {
        RegisterOnline();
        qInfo() << "Authenticated:" << npid;
    }
    return ErrorType::NoError;
}

void ClientSession::RegisterOnline() {
    if (m_registered || !m_pendingOnline || !m_authenticated)
        return;
    const PendingOnline pending = *m_pendingOnline;
    m_pendingOnline.reset();
    m_registered = true;

    const int64_t userId = m_info.userId;
    const QString npid = m_info.npid;
    // Read again: friendships may have changed while the login was held.
    const UserRelationships rels = m_db->GetRelationships(userId);

    // Register session and notify online friends
    QVector<std::tuple<std::function<void(QByteArray)>, QString, int64_t>> onlineFriendSenders;
    {
        QWriteLocker lk(&m_shared->clientsLock);
        m_shared->pendingVersionLogins.remove(userId);
        SharedState::ClientEntry entry;
        entry.npid = npid;
        entry.appearOffline = pending.appearOffline;
        // The admin API may have changed it while the login was held.
        entry.matching2Access = m_db->GetMatching2Access(userId);
        // Title context reported by the emulator at login -> presence gameTitleInfo.
        entry.npTitleId = pending.titleId;
        entry.titleName = pending.titleName;
        entry.clientVersion = m_reportedVersion;
        entry.versionReported = m_versionReported;
        entry.send = [this](QByteArray pkt) {
            QMetaObject::invokeMethod(
                this, [this, pkt]() { SendPacket(pkt); }, Qt::QueuedConnection);
        };
        entry.resetMatchingRoomState = [this](uint64_t roomId) {
            QMetaObject::invokeMethod(
                this, [this, roomId]() { ResetMatchingRoomState(roomId); }, Qt::QueuedConnection);
        };
        entry.disconnect = [this]() {
            QMetaObject::invokeMethod(m_socket, "disconnectFromHost", Qt::QueuedConnection);
        };
        for (const auto& [friendId, friendNpid] : rels.friends) {
            entry.friends.insert(friendId, friendNpid);
            auto it = m_shared->clients.find(friendId);
            if (it != m_shared->clients.end()) {
                it->friends.insert(userId, npid);
                onlineFriendSenders.append({it->send, friendNpid, friendId});
            }
        }
        m_shared->clients[userId] = std::move(entry);
        m_shared->npidToUserId[npid] = userId;
        // Outlives the session entry above so the WebAPI can still name the title.
        if (!pending.titleId.isEmpty())
            m_shared->lastLoginTitleId[userId] = pending.titleId;
    }

    // Count this authenticated session in live usage stats
    m_shared->UsageOnLogin();

    // Notify online friends we came online -- unless Appear-Offline is set, in which case
    // we stay invisible (handled as offline by everyone else).
    if (!onlineFriendSenders.isEmpty() && !pending.appearOffline) {
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
        // WebApi onlineStatus presence update: service name None (basic push) + exact
        // dataType match the system "npweblis" listener's filter. from = us, to = each
        // recipient
        for (const auto& [send, friendNpid, friendId] : onlineFriendSenders) {
            send(pkt);
            send(ClientSession::BuildNotification(
                NotificationType::WebApiPushEvent,
                ClientSession::BuildWebApiPushPayload(
                    QString(), 0, QStringLiteral("np:service:presence:onlineStatus"), QByteArray(),
                    npid, friendNpid, {}, userId, friendId)));
        }
    }

    // A game seen while the login was held: same-game friends learn the title now.
    bool inGame = false;
    {
        QReadLocker ul(&m_shared->usageLock);
        inGame = m_shared->usageClientGame.contains(userId);
    }
    if (inGame)
        EmitPresenceGameTitleInfo();
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

    PurgeSummary summary;
    int blobsDeleted = 0;
    int cachedScoresDropped = 0;
    if (!DeleteAccountAndArtifacts(*m_db, m_shared, userOpt->userId, summary, blobsDeleted,
                                   cachedScoresDropped)) {
        qCritical() << "CmdDelete: DB error deleting" << npid;
        return ErrorType::DbFail;
    }

    qInfo().nospace() << "Account deleted: " << npid << " (scores: " << summary.scores
                      << ", tus variables: " << summary.tusVariables
                      << ", tus data: " << summary.tusData
                      << ", relationships: " << summary.friendships
                      << ", score blobs: " << blobsDeleted << ")";

    if (userOpt->userId == m_info.userId) {
        QWriteLocker lk(&m_shared->clientsLock);
        m_shared->clients.remove(m_info.userId);
        m_authenticated = false;
    } else {
        // An admin deleted somebody else. If that account is connected, close its
        // session too
        std::function<void()> drop;
        {
            QReadLocker lk(&m_shared->clientsLock);
            const auto it = m_shared->clients.constFind(userOpt->userId);
            if (it != m_shared->clients.constEnd())
                drop = it->disconnect;
        }
        if (drop)
            drop();
    }

    QMetaObject::invokeMethod(m_socket, "disconnectFromHost", Qt::QueuedConnection);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetToken(QByteArray& reply) {
    if (m_info.token.isEmpty()) {
        qWarning() << "CmdGetToken: token empty for userId=" << m_info.userId
                   << "npid=" << m_info.npid;
        return ErrorType::DbFail;
    }

    shadnet::GetTokenReply pb;
    pb.set_token(m_info.token.toStdString());
    pb.set_user_id(static_cast<uint64_t>(m_info.userId));
    pb.set_npid(m_info.npid.toStdString());
    appendProto(reply, pb);

    qInfo() << "CmdGetToken: served token for" << m_info.npid;
    return ErrorType::NoError;
}

// LookupOnlineId
// Request:  u32LE blob size + LookupOnlineIdRequest proto
// Reply:    ErrorType(u8) + u32LE blob size + LookupOnlineIdReply proto
//
// Global online ID to account ID resolution
ErrorType ClientSession::CmdLookupOnlineId(StreamExtractor& data, QByteArray& reply) {
    shadnet::LookupOnlineIdRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const QString npid = QString::fromStdString(req.npid());
    if (!IsValidNpid(npid))
        return ErrorType::InvalidInput;

    const auto userId = m_db->GetUserId(npid);
    if (!userId.has_value()) {
        qInfo() << "CmdLookupOnlineId:" << npid << "-> not found";
        return ErrorType::NotFound;
    }

    const QString canonicalNpid = m_db->GetUsername(*userId).value_or(npid);

    shadnet::LookupOnlineIdReply pb;
    pb.set_account_id(static_cast<uint64_t>(*userId));
    pb.set_npid(canonicalNpid.toStdString());
    appendProto(reply, pb);

    qInfo() << "CmdLookupOnlineId:" << canonicalNpid << "->" << *userId;
    return ErrorType::NoError;
}
