// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <optional>

#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <QObject>
#include <QReadWriteLock>
#include <QTcpServer>

#include "config.h"
#include "database.h"

struct SharedState;

class MemberApiServer : public QObject {
    Q_OBJECT
public:
    explicit MemberApiServer(QObject* parent = nullptr);
    ~MemberApiServer() override;

    bool Start(ConfigManager* config, const QString& dbPath, SharedState* shared);

private:
    struct MemberSession {
        int64_t userId = 0;
        QString npid;
        QDateTime expires;
    };

    void RegisterRoutes();
    bool CheckApiKey(const QHttpServerRequest& req) const;
    QHttpServerResponse ApiKeyError(const QHttpServerRequest& req) const;
    std::optional<MemberSession> Authenticate(const QHttpServerRequest& req);
    QString IssueToken(int64_t userId, const QString& npid);
    void RevokeToken(const QString& token);
    void RevokeSessionsFor(int64_t userId);
    void PruneExpiredSessions();
    bool IsThrottled(const QString& peer, int& retryAfterSecs);
    void RegisterFailure(const QString& peer);
    void ClearFailures(const QString& peer);

    ConfigManager* m_config = nullptr;
    SharedState* m_shared = nullptr;
    QString m_dbPath;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QHttpServer> m_http;
    std::unique_ptr<QTcpServer> m_tcp;

    mutable QReadWriteLock m_sessionsLock;
    QHash<QString, MemberSession> m_sessions;

    struct FailureRecord {
        int count = 0;
        QDateTime firstAt;
        QDateTime blockedUntil;
    };
    mutable QReadWriteLock m_failuresLock;
    QHash<QString, FailureRecord> m_failures;
};
