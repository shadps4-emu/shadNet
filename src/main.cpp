#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QCommandLineParser>
#include "config.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("shadnet");

    // Set working directory to executable location
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    qInfo() << "ShadNet Qt server";

    ConfigManager config;
    config.Load();
    config.LoadBannedDomains();


    return app.exec();
}
