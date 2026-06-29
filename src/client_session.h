// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <QAtomicInt>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QSslSocket>
#include <QTcpSocket>
#include <database.h>
#include "config.h"
#include "matching_types.h"
#include "protocol.h"
#include "score_cache.h"
#include "score_db.h"
#include "score_files.h"
#include "stream_extractor.h"

// Shared state visible to all sessions (thread-safe with locks)
struct SharedState {
    ConfigManager* config;
    ScoreCache* scoreCache = nullptr;
    ScoreFiles* scoreFiles = nullptr;
    // Connected clients: userId to (npid, channel write function)
    mutable QReadWriteLock clientsLock;
    struct ClientEntry {
        QString npid;
        std::function<void(QByteArray)> send;
        std::function<void(uint64_t)> resetMatchingRoomState;
        // Online friends for this session: userId → npid.
        // Protected by clientsLock (same lock as the outer clients map).
        QHash<int64_t, QString> friends;
    };
    QHash<int64_t, ClientEntry> clients;
    QHash<QString, int64_t> npidToUserId; // reverse lookup, protected by clientsLock

    // Matchmaking shared state
    MatchingSharedState matching;

    // Live usage stats for the read-only stats HTTP server
    mutable QReadWriteLock usageLock;
    int usageTotalOnline = 0;                // authenticated sessions currently online
    QHash<QString, int> usageGameUsers;      // comId -> active session count
    QHash<int64_t, QString> usageClientGame; // userId -> last-seen comId

    void UsageOnLogin() {
        QWriteLocker lk(&usageLock);
        ++usageTotalOnline;
    }
    void UsageOnLogout(int64_t userId) {
        QWriteLocker lk(&usageLock);
        if (usageTotalOnline > 0)
            --usageTotalOnline;
        auto it = usageClientGame.find(userId);
        if (it != usageClientGame.end()) {
            auto g = usageGameUsers.find(it.value());
            if (g != usageGameUsers.end() && --g.value() <= 0)
                usageGameUsers.erase(g);
            usageClientGame.erase(it);
        }
    }
    void UsageTouchGame(int64_t userId, const QString& comId) {
        if (comId.isEmpty())
            return;
        QWriteLocker lk(&usageLock);
        auto it = usageClientGame.find(userId);
        if (it != usageClientGame.end()) {
            if (it.value() == comId)
                return;
            auto old = usageGameUsers.find(it.value());
            if (old != usageGameUsers.end() && --old.value() <= 0)
                usageGameUsers.erase(old);
            it.value() = comId;
        } else {
            usageClientGame.insert(userId, comId);
        }
        ++usageGameUsers[comId];
    }
};

// Per-connection session info
struct ClientInfo {
    int64_t userId = 0;
    QString npid;
    QString avatarUrl;
    QString token;
    bool admin = false;
    bool statAgent = false;
    bool banned = false;
};

class ClientSession : public QObject {
    Q_OBJECT
public:
    explicit ClientSession(QTcpSocket* socket, SharedState* shared, const QString& dbPath,
                           bool isSsl = true, QObject* parent = nullptr);
    ~ClientSession();

    void Start();
    void CleanupOnDisconnect();
    void SendPacket(const QByteArray& pkt);
    static bool IsValidNpid(const QString& npid);
    Database& db() {
        return *m_db;
    }

    // commands cmd_account.cpp
    ErrorType CmdCreate(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdLogin(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdDelete(StreamExtractor& data);

    // commands cmd_friend.cpp
    ErrorType CmdAddFriend(StreamExtractor& data);
    ErrorType CmdRemoveFriend(StreamExtractor& data);
    ErrorType CmdAddBlock(StreamExtractor& data);
    ErrorType CmdRemoveBlock(StreamExtractor& data);

    // commands cmd_score.cpp
    ErrorType CmdGetBoardInfos(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdRecordScore(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdRecordScoreData(StreamExtractor& data);
    ErrorType CmdGetScoreData(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetScoreRange(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetScoreFriends(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetScoreNpid(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetScoreAccountId(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetScoreGameDataByAccId(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetToken(QByteArray& reply);

    // commands cmd_matching.cpp
    ErrorType CmdContextStart(StreamExtractor& data);
    ErrorType CmdContextStop(StreamExtractor& data);
    ErrorType CmdCreateRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdJoinRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdLeaveRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSearchRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdRequestSignalingInfos(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSetRoomDataInternal(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSetRoomDataExternal(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdKickoutRoomMember(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetWorldInfoList(StreamExtractor& data, QByteArray& reply);

signals:
    void Disconnected();

private slots:
    void OnReadyRead();
    void OnDisconnected();

private:
    void ProcessPacket(uint16_t command, uint64_t packetId, const QByteArray& payload);
    ErrorType DispatchCommand(CommandType cmd, StreamExtractor& se, QByteArray& reply);

    // Notification helpers
    void SendNotification(NotificationType type, const QByteArray& payload, int64_t targetUserId);
    void SendSelfNotification(NotificationType type, const QByteArray& payload);
    static QByteArray BuildNotification(NotificationType type, const QByteArray& payload);

    // Build the length-prefixed payload for a WebApiPushEvent notification. Exposed so
    // fan-out sites that already hold per-recipient send() lambdas (presence) can reuse it.
    static QByteArray BuildWebApiPushPayload(const QString& npServiceName, quint32 npServiceLabel,
                                             const QString& dataType, const QByteArray& data,
                                             const QString& fromNpid, const QString& toNpid);
    // Push a generic NP WebApi push event to one online user. The emulator forwards it
    // verbatim to libSceNpWebApi push-event callbacks. data may be empty (the listener
    // re-fetches via the REST routes); from/to npids may be empty.
    void PushWebApiEvent(const QString& npServiceName, quint32 npServiceLabel,
                         const QString& dataType, const QByteArray& data, const QString& fromNpid,
                         const QString& toNpid, int64_t targetUserId);

    // Matching helpers (cmd_matching.cpp)
    void SendMatchingNotification(NotificationType type, const QByteArray& payload,
                                  const QString& targetNpid);
    void NotifyRoomMembers(NotificationType type, const QByteArray& payload,
                           const QString& matchingKey, uint64_t roomId,
                           const QString& excludeNpid = {});
    void SendRoomMemberEvent(uint64_t roomId, uint32_t event, uint32_t cause,
                             const RoomMember& member, const QString& excludeNpid = {});
    void SendRoomEventToTarget(uint64_t roomId, uint32_t event, uint32_t cause, int32_t errorCode,
                               const QString& targetNpid);
    void DoLeaveRoom(uint64_t roomId);
    void CleanupMatchingOnDisconnect();
    void ResetMatchingRoomState(uint64_t roomId);
    void GetSelfSignalingAddr(QString& addr, uint16_t& port) const;

    QTcpSocket* m_socket;
    bool m_isSsl = true;
    SharedState* m_shared;
    bool m_authenticated = false;
    QByteArray m_readBuf;
    std::unique_ptr<Database> m_db;
    ClientInfo m_info;
    MatchingSessionState m_matching;
};

inline bool ClientSession::IsValidNpid(const QString& npid) {
    if (npid.size() < 3 || npid.size() > 16)
        return false;
    for (const QChar& c : npid)
        if (!c.isLetterOrNumber() && c != '-' && c != '_')
            return false;
    if (npid == "DeletedUser")
        return false;
    return true;
}
