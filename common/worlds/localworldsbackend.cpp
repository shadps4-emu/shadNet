// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include "localworldsbackend.h"
#include "worldsstore.h"

LocalWorldsBackend::LocalWorldsBackend(const QString& path, QObject* parent)
    : WorldsBackend(parent), m_path(worldsFilePath(path)) {}
void LocalWorldsBackend::load() {
    request(QStringLiteral("read"), {});
}
void LocalWorldsBackend::save(const QString& content, const QString& revision, bool apply) {
    request(QStringLiteral("save"), {{QStringLiteral("content"), content},
                                     {QStringLiteral("revision"), revision},
                                     {QStringLiteral("reload"), apply}});
}
void LocalWorldsBackend::reload(const QString& revision) {
    request(QStringLiteral("reload"), {{QStringLiteral("revision"), revision}});
}

void LocalWorldsBackend::offline(const QString& operation, const QJsonObject& body) {
    QLockFile running(m_path + QStringLiteral(".running"));
    if (!running.tryLock(0)) {
        emit failed(
            tr("The server is running but its local editor connection is unavailable, or the "
               "directory is not writable. Check that both tools run under the same OS account."),
            true);
        return;
    }
    if (operation == QLatin1String("reload") || body.value(QStringLiteral("reload")).toBool()) {
        emit failed(
            tr("The local server is not running. Read the file again to save without reloading."),
            true);
        return;
    }
    WorldsFileTransaction transaction(m_path);
    WorldsSnapshot snapshot;
    QString error;
    int status = 500;
    bool ok = operation == QLatin1String("save")
                  ? transaction.write(body.value(QStringLiteral("content")).toString(),
                                      body.value(QStringLiteral("revision")).toString(), snapshot,
                                      error, status)
                  : transaction.read(snapshot, error);
    if (ok)
        emit received(snapshot);
    else
        emit failed(error, status == 409);
}

void LocalWorldsBackend::request(const QString& operation, const QJsonObject& body) {
    auto* socket = new QLocalSocket(this);
    auto* timer = new QTimer(socket);
    timer->setSingleShot(true);
    struct Pending {
        bool connected = false;
        bool done = false;
        QByteArray buffer;
    };
    auto pending = std::make_shared<Pending>();
    connect(timer, &QTimer::timeout, this, [this, socket, pending]() {
        if (pending->done)
            return;
        pending->done = true;
        socket->abort();
        socket->deleteLater();
        emit failed(
            tr("Local request timed out. Read again to check whether the change was applied."),
            true);
    });
    connect(socket, &QLocalSocket::connected, this, [socket, pending, operation, body]() {
        pending->connected = true;
        QJsonObject request = body;
        request.insert(QStringLiteral("operation"), operation);
        socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    });
    connect(socket, &QLocalSocket::readyRead, this, [this, socket, pending, timer]() {
        if (pending->done)
            return;
        pending->buffer.append(socket->readAll());
        if (pending->buffer.size() > kMaxWorldsBytes * 12 + 4096) {
            pending->done = true;
            socket->abort();
            socket->deleteLater();
            emit failed(tr("The local server returned an oversized response."), true);
            return;
        }
        const int end = pending->buffer.indexOf('\n');
        if (end < 0)
            return;
        pending->done = true;
        timer->stop();
        const auto json = QJsonDocument::fromJson(pending->buffer.left(end)).object();
        WorldsSnapshot snapshot;
        if (json.value(QStringLiteral("ok")).toBool() &&
            worldsSnapshotFromJson(json.value(QStringLiteral("snapshot")).toObject(), snapshot))
            emit received(snapshot);
        else
            emit failed(json.value(QStringLiteral("message"))
                            .toString(tr("Invalid response from the local server.")),
                        json.value(QStringLiteral("status")).toInt() == 409 ||
                            json.value(QStringLiteral("ok")).toBool());
        socket->disconnectFromServer();
        socket->deleteLater();
    });
    connect(socket, &QLocalSocket::errorOccurred, this,
            [this, socket, pending, timer, operation, body](QLocalSocket::LocalSocketError error) {
                if (pending->done)
                    return;
                pending->done = true;
                timer->stop();
                socket->deleteLater();
                if (!pending->connected && (error == QLocalSocket::ServerNotFoundError ||
                                            error == QLocalSocket::ConnectionRefusedError)) {
                    offline(operation, body);
                    return;
                }
                emit failed(tr("Local connection failed: %1. Read again before retrying.")
                                .arg(socket->errorString()),
                            true);
            });
    timer->start(10000);
    socket->connectToServer(worldsSocketName(m_path));
}
