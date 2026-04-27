// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_server.h"

#include <QDebug>
#include <QHostAddress>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>

#include "webapi_auth.h"

WebApiServer::WebApiServer(QObject* parent) : QObject(parent) {}
WebApiServer::~WebApiServer() = default;

bool WebApiServer::Start(ConfigManager* config, const QString& dbPath) {
    m_config = config;

    // Open a dedicated DB connection for this thread
    m_db = std::make_unique<Database>(QStringLiteral("webapi_main"));
    if (!m_db->Open(dbPath)) {
        qCritical() << "WebApiServer: failed to open database at" << dbPath;
        return false;
    }

    m_http = std::make_unique<QHttpServer>(this);
    RegisterRoutes();
    m_tcp = std::make_unique<QTcpServer>(this);

    const QString host = m_config->GetHost();
    const quint16 port = m_config->GetWebApiPort().toUShort();
    if (!m_tcp->listen(QHostAddress(host), port)) {
        qCritical() << "WebApiServer: failed to bind" << host << ":" << port << "—"
                    << m_tcp->errorString();
        return false;
    }

    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "WebApiServer: QHttpServer failed to attach to listener";
        return false;
    }

    qInfo() << "WebApiServer listening on" << host << ":" << port;
    return true;
}

void WebApiServer::RegisterRoutes() {
    m_http->route("/status", [](const QHttpServerRequest&) {
        QJsonObject body;
        body.insert("ok", true);
        body.insert("service", "shadnet-webapi");
        return QHttpServerResponse{"application/json",
                                   QJsonDocument(body).toJson(QJsonDocument::Compact),
                                   QHttpServerResponse::StatusCode::Ok};
    });
}
