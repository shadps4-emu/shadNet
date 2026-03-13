#pragma once
#include <memory>
#include <QList>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QTcpServer>
#include "client_session.h"
#include "config.h"

class ShadNetServer : public QObject {
    Q_OBJECT
public:
    explicit ShadNetServer(QObject* parent = nullptr);
    ~ShadNetServer();

    bool Start(ConfigManager* config);
    void Stop();

private slots:
    void OnNewUnsecuredConnection();

private:
    void SpawnSession(QTcpSocket* socket, bool isSsl);

    ConfigManager* m_config = nullptr;
    QTcpServer* m_unsecuredServer = nullptr; // plain TCP connections
    SharedState m_shared;

    QString m_dbPath;
};
