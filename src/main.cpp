// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include "admin_api.h"
#include "config.h"
#include "member_api.h"
#include "server.h"
#include "version.h"
#include "webapi_server.h"

int main(int argc, char* argv[]) {
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n*.info=true\n*.warning=true"));
    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-dd HH:mm:ss.zzz}  "
                                      "%{if-debug}DEBUG%{endif}"
                                      "%{if-info} INFO%{endif}"
                                      "%{if-warning} WARN%{endif}"
                                      "%{if-critical} CRIT%{endif}"
                                      "%{if-fatal}FATAL%{endif}"
                                      "  %{if-category}[%{category}] %{endif}%{message}"));

    QCoreApplication app(argc, argv);
    app.setApplicationName("shadnet");

    // Set working directory to executable location
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    qInfo().noquote() << "ShadNet Qt server version" << ShadNet::Version() << "(built "
                      << ShadNet::BuildTimestamp() << ")";

    ConfigManager config;
    config.Load();
    config.LoadBannedDomains();

    ShadNetServer server;
    if (!server.Start(&config)) {
        qCritical() << "Failed to start server";
        return 1;
    }

    // Start the HTTP/JSON WebAPI listener alongside the binary protocol.
    WebApiServer webapi;
    if (!webapi.Start(&config, "db/shadnet.db", &server.Shared())) {
        qWarning() << "WebApiServer failed to start; continuing without WebAPI";
    }

    MemberApiServer memberApi;
    if (config.IsMemberApiEnabled()) {
        if (!memberApi.Start(&config, "db/shadnet.db", &server.Shared()))
            qWarning() << "MemberApiServer failed to start; continuing without the member API";
    } else {
        qInfo() << "Member API disabled (MemberApiEnabled=false)";
    }

    // Start the admin API the shadNet admin tool talks to.
    AdminApiServer adminApi;
    if (config.IsAdminApiEnabled()) {
        if (!adminApi.Start(&config, "db/shadnet.db", &server.Shared())) {
            qWarning() << "AdminApiServer failed to start; continuing without admin API";
        }
    } else {
        qInfo() << "Admin API disabled (AdminApiEnabled=false)";
    }

    return app.exec();
}
