// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include "worlds_service.h"
#include "worldsstore.h"

WorldsService::WorldsService(const QString& path, MatchingSharedState* matching, QObject* parent)
    : QObject(parent), m_path(worldsFilePath(path)), m_matching(matching),
      m_runningLock(m_path + QStringLiteral(".running")) {
    connect(&m_local, &QLocalServer::newConnection, this, &WorldsService::AcceptLocalConnections);
}

bool WorldsService::Start(QString& error) {
    if (!m_runningLock.tryLock(0)) {
        error = QStringLiteral(
            "Another server owns this worlds.cfg, or the directory is not writable.");
        return false;
    }
    WorldsFileTransaction transaction(m_path);
    WorldsSnapshot disk;
    if (!transaction.read(disk, error))
        return false;
    const auto parsed = parseWorldsConfig(disk.content);
    if (!parsed.isValid()) {
        error = QStringLiteral("worlds.cfg line %1: %2")
                    .arg(parsed.errors.first().line)
                    .arg(parsed.errors.first().message);
        return false;
    }
    {
        QWriteLocker lock(&m_matching->roomsLock);
        m_matching->worldConfigs.clear();
        m_matching->titleGroups.clear();
        for (const auto& world : parsed.worlds)
            m_matching->worldConfigs[world.group].append(
                {world.worldId, world.serverId, world.lobbies, world.maxLobbyMembers});
        for (const auto& mapping : parsed.groups)
            m_matching->titleGroups[mapping.titleId] = mapping.group;
        m_activeContent = disk.content;
        m_activeRevision = disk.revision;
    }
    m_local.setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = worldsSocketName(m_path);
    if (!m_local.listen(name)) {
        // The lifetime lock proves no other cooperating server owns this path.
        // Unix sockets can remain after a crash; Windows removeServer is a no-op.
        QLocalServer::removeServer(name);
        if (!m_local.listen(name)) {
            qWarning() << "Local worlds editor unavailable:" << m_local.errorString();
        }
    }
    return true;
}

WorldsResult WorldsService::Handle(const QString& operation, const QJsonObject& body) {
    QMutexLocker serialize(&m_mutex);
    WorldsResult result;
    if (operation != QLatin1String("read") && operation != QLatin1String("save") &&
        operation != QLatin1String("reload")) {
        result.status = 400;
        result.message = QStringLiteral("Unknown worlds operation.");
        return result;
    }
    const bool saving = operation == QLatin1String("save");
    const bool mutation = operation != QLatin1String("read");
    if (mutation && (!body.value(QStringLiteral("revision")).isString() ||
                     body.value(QStringLiteral("revision")).toString().isEmpty())) {
        result.status = 400;
        result.message = QStringLiteral("A saved-file revision is required.");
        return result;
    }
    if (saving && (!body.value(QStringLiteral("content")).isString() ||
                   !body.value(QStringLiteral("reload")).isBool())) {
        result.status = 400;
        result.message = QStringLiteral("Save requires string content and a boolean reload flag.");
        return result;
    }
    WorldsFileTransaction transaction(m_path);
    WorldsSnapshot disk;
    if (!transaction.read(disk, result.message))
        return result;
    if (!mutation) {
        disk.activeContent = m_activeContent;
        disk.activeRevision = m_activeRevision;
        disk.canReload = true;
        return {true, 200, QString(), disk};
    }
    if (disk.revision != body.value(QStringLiteral("revision")).toString()) {
        result.status = 409;
        result.message = QStringLiteral("worlds.cfg changed since it was read. Read the latest "
                                        "version before applying changes.");
        return result;
    }
    const bool apply = !saving || body.value(QStringLiteral("reload")).toBool();
    const QString content =
        saving ? body.value(QStringLiteral("content")).toString() : disk.content;
    const auto parsed = parseWorldsConfig(content);
    if (!parsed.isValid()) {
        result.status = 422;
        result.message = QStringLiteral("Line %1: %2")
                             .arg(parsed.errors.first().line)
                             .arg(parsed.errors.first().message);
        return result;
    }
    QHash<QString, QVector<WorldConfig>> worlds;
    QHash<QString, QString> groups;
    for (const auto& world : parsed.worlds)
        worlds[world.group].append(
            {world.worldId, world.serverId, world.lobbies, world.maxLobbyMembers});
    for (const auto& mapping : parsed.groups)
        groups[mapping.titleId] = mapping.group;

    // Room creation/removal uses this same lock. Allocate the candidate first;
    // do not clear room indexes, sessions, or sockets when swapping config maps.
    QWriteLocker roomLock(&m_matching->roomsLock);
    if (apply) {
        for (auto it = m_matching->rooms.cbegin(); it != m_matching->rooms.cend(); ++it) {
            const QString group = it.key().first;
            const auto candidate = worlds.value(group);
            const bool wasExplicit = !m_matching->worldConfigs.value(group).isEmpty();
            bool visible = candidate.isEmpty() && !wasExplicit;
            for (const auto& world : candidate)
                visible |= world.worldId == it->worldId && world.serverId == it->serverId;
            if (!visible) {
                result.status = 422;
                result.message = QStringLiteral("World %1 in %2 still contains rooms. Keep its "
                                                "world and server IDs until those rooms close.")
                                     .arg(it->worldId)
                                     .arg(group);
                return result;
            }
        }
    }
    if (saving && !transaction.write(content, disk.revision, disk, result.message, result.status))
        return result;
    if (apply) {
        m_matching->worldConfigs.swap(worlds);
        m_matching->titleGroups.swap(groups);
        m_activeContent = content;
        m_activeRevision = disk.revision;
    }
    disk.activeContent = m_activeContent;
    disk.activeRevision = m_activeRevision;
    disk.canReload = true;
    return {true, 200, QString(), disk};
}

void WorldsService::AcceptLocalConnections() {
    while (auto* socket = m_local.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        auto done = std::make_shared<bool>(false);
        socket->setReadBufferSize(kMaxWorldsBytes * 6 + 4096);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(10000, socket, [socket]() {
            socket->abort();
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer, done]() {
            if (*done)
                return;
            buffer->append(socket->readAll());
            if (buffer->size() > kMaxWorldsBytes * 6 + 2048) {
                *done = true;
                socket->abort();
                socket->deleteLater();
                return;
            }
            const int end = buffer->indexOf('\n');
            if (end < 0)
                return;
            *done = true;
            const auto doc = QJsonDocument::fromJson(buffer->left(end));
            const QJsonObject body = doc.object();
            const QString operation = body.value(QStringLiteral("operation")).toString();
            WorldsResult result =
                doc.isObject()
                    ? Handle(operation, body)
                    : WorldsResult{false, 400, QStringLiteral("Invalid JSON request."), {}};
            if (operation != QLatin1String("read"))
                qInfo() << "Local worlds" << operation << (result.ok ? "succeeded" : "failed")
                        << "revision" << result.snapshot.revision << result.message;
            QJsonObject response{{QStringLiteral("ok"), result.ok},
                                 {QStringLiteral("status"), result.status},
                                 {QStringLiteral("message"), result.message},
                                 {QStringLiteral("snapshot"), worldsSnapshotJson(result.snapshot)}};
            socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
            socket->disconnectFromServer();
        });
    }
}
