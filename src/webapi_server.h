// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <QHttpServer>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include "config.h"
#include "database.h"

class WebApiServer : public QObject {
    Q_OBJECT
public:
    explicit WebApiServer(QObject* parent = nullptr);
    ~WebApiServer();

    // Bind, register routes, and start listening.
    // dbPath is the same SQLite file used by ShadNetServer / ClientSession.
    bool Start(ConfigManager* config, const QString& dbPath);

private:
    void RegisterRoutes();
    ConfigManager* m_config = nullptr;
    std::unique_ptr<QHttpServer> m_http;
    std::unique_ptr<QTcpServer> m_tcp;
    std::unique_ptr<Database> m_db;
};
