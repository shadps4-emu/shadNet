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
#include "score_cache.h"
#include "score_files.h"

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
    bool InitScoreSystem();
    bool LoadScoreboardsCfg(const QString& path);

    ConfigManager* m_config = nullptr;
    QTcpServer* m_unsecuredServer = nullptr; // plain TCP connections
    SharedState m_shared;
    QString m_dbPath;

    std::unique_ptr<ScoreCache> m_scoreCache;
    std::unique_ptr<ScoreFiles> m_scoreFiles;
};
