// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include "config.h"
#include "server.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("shadnet");

    // Set working directory to executable location
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    qInfo() << "ShadNet Qt server";

    ConfigManager config;
    config.Load();
    config.LoadBannedDomains();

    ShadNetServer server;
    if (!server.Start(&config)) {
        qCritical() << "Failed to start server";
        return 1;
    }

    return app.exec();
}
