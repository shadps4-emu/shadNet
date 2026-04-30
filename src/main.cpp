// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include "config.h"
#include "server.h"
#include "webapi_server.h"

const QString versionString = QStringLiteral("0.0.2");

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

    qInfo() << "ShadNet Qt server version" << versionString;

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
    if (!webapi.Start(&config, "db/shadnet.db")) {
        qWarning() << "WebApiServer failed to start; continuing without WebAPI";
    }

    return app.exec();
}
