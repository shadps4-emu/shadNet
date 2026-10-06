// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include "localworldsbackend.h"
#include "member_window.h"
#include "worldsdialog.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("shadNet");
    app.setApplicationName("shadNet Toolbox");

    // Keep existing file selections when upgrading to the renamed application.
    QSettings settings;
    const QSettings legacySettings("shadNet", "Member Manager");
    for (const auto* key : {"databasePath", "worldsPath"}) {
        if (!settings.contains(key) && legacySettings.contains(key))
            settings.setValue(key, legacySettings.value(key));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("shadNet Toolbox: local account and Worlds tools.");
    parser.addHelpOption();
    parser.addOption({"worlds", "Open a local worlds.cfg directly.", "path"});
    parser.addPositionalArgument("database", "Optional path to the local member database.");
    parser.process(app);
    if (parser.isSet("worlds")) {
        LocalWorldsBackend backend(parser.value("worlds"));
        WorldsDialog dialog(&backend);
        dialog.exec();
        return 0;
    }
    QString path;
    if (!parser.positionalArguments().isEmpty()) {
        path = parser.positionalArguments().first();
    } else {
        const QDir executable(app.applicationDirPath());
        const QStringList candidates = {
            QSettings().value("databasePath").toString(),
            executable.filePath("db/shadnet.db"),
            executable.filePath("../db/shadnet.db"),
        };
        for (const QString& candidate : candidates) {
            if (QFileInfo(candidate).isFile()) {
                path = candidate;
                break;
            }
        }
    }
    MemberWindow window(path);
    window.show();
    return app.exec();
}
