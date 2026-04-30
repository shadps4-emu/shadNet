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
#include "matching.h"
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
    ErrorType CmdRegisterHandlers(StreamExtractor& data);
    ErrorType CmdCreateRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdJoinRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdLeaveRoom(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdGetRoomList(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdRequestSignalingInfos(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSignalingEstablished(StreamExtractor& data);
    ErrorType CmdActivationConfirm(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdCancelActivationIntent(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSetRoomDataInternal(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdSetRoomDataExternal(StreamExtractor& data, QByteArray& reply);
    ErrorType CmdKickoutRoomMember(StreamExtractor& data, QByteArray& reply);

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

    // Matching helpers (cmd_matching.cpp)
    void SendMatchingNotification(NotificationType type, const QByteArray& payload,
                                  const QString& targetNpid);
    void NotifyRoomMembers(NotificationType type, const QByteArray& payload, uint64_t roomId,
                           const QString& excludeNpid = {});
    void DoLeaveRoom(uint64_t roomId);
    void CleanupMatchingOnDisconnect();
    void ResetMatchingRoomState(uint64_t roomId);

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
