// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "clientversions_service.h"

#include <functional>
#include <QDebug>
#include <QVector>
#include "client_session.h"
#include "client_version_rules.h"
#include "clientversionsconfig.h"
#include "worldsstore.h"

namespace {

const QString kLabel = QStringLiteral("clientversions.cfg");

QString Validate(const QString& content) {
    const auto parsed = parseClientVersionsConfig(content);
    return parsed.isValid() ? QString()
                            : QStringLiteral("Line %1: %2")
                                  .arg(parsed.errors.first().line)
                                  .arg(parsed.errors.first().message);
}

} // namespace

ClientVersionsService::ClientVersionsService(const QString& path, ClientVersionRules* rules,
                                             SharedState* shared)
    : m_path(worldsFilePath(path)), m_rules(rules), m_shared(shared) {}

void ClientVersionsService::Start() {
    WorldsFileTransaction transaction(m_path, kLabel, Validate);
    WorldsSnapshot disk;
    QString error;
    if (transaction.read(disk, error)) {
        m_activeContent = disk.content;
        m_activeRevision = disk.revision;
    } else {
        qWarning() << "clientversions.cfg editor:" << error;
    }
}

void ClientVersionsService::Apply(const QString& content, bool recheckOnline,
                                  ClientVersionsResult& result) {
    QStringList errors;
    m_rules->Apply(content, errors);
    result.applied = true;
    result.rulesApplied = m_rules->RuleCount();
    {
        QMutexLocker lk(&m_shared->versionCooldownMutex);
        result.cooldownsCleared = static_cast<int>(m_shared->versionCooldownUntil.size());
        m_shared->versionCooldownUntil.clear();
    }

    if (!recheckOnline)
        return;
    QVector<std::function<void()>> rechecks;
    {
        QReadLocker lk(&m_shared->clientsLock);
        for (auto it = m_shared->clients.constBegin(); it != m_shared->clients.constEnd(); ++it) {
            if (!it->recheckVersion)
                continue; // exempt (stat agents)
            rechecks.append(it->recheckVersion);
            if (!m_rules->Evaluate(it->clientVersion).allowed)
                ++result.wouldDeny;
        }
    }
    for (const auto& recheck : rechecks)
        recheck(); // queued onto each session's thread
    result.rechecked = static_cast<int>(rechecks.size());
}

ClientVersionsResult ClientVersionsService::Handle(const QString& operation,
                                                   const QJsonObject& body) {
    QMutexLocker serialize(&m_mutex);
    ClientVersionsResult result;
    if (operation != QLatin1String("read") && operation != QLatin1String("save") &&
        operation != QLatin1String("reload")) {
        result.status = 400;
        result.message = QStringLiteral("Unknown client version rules operation.");
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
    const QJsonValue recheck = body.value(QStringLiteral("recheckOnline"));
    if (mutation && !recheck.isUndefined() && !recheck.isBool()) {
        result.status = 400;
        result.message = QStringLiteral("recheckOnline must be a boolean.");
        return result;
    }
    WorldsFileTransaction transaction(m_path, kLabel, Validate);
    WorldsSnapshot disk;
    if (!transaction.read(disk, result.message))
        return result;
    auto finish = [&]() {
        disk.activeContent = m_activeContent;
        .disk.activeRevision = m_activeRevision.isEmpty() ? disk.revision : m_activeRevision;
        disk.canReload = true;
        result.ok = true;
        result.status = 200;
        result.snapshot = disk;
        result.rulesApplied = m_rules->RuleCount();
    };
    if (!mutation) {
        finish();
        return result;
    }
    if (disk.revision != body.value(QStringLiteral("revision")).toString()) {
        result.status = 409;
        result.message = QStringLiteral("clientversions.cfg changed since it was read. Read the "
                                        "latest version before applying changes.");
        return result;
    }
    const bool apply = !saving || body.value(QStringLiteral("reload")).toBool();
    const QString content =
        saving ? body.value(QStringLiteral("content")).toString() : disk.content;
    if (const QString invalid = Validate(content); !invalid.isEmpty()) {
        result.status = 422;
        result.message = invalid;
        return result;
    }
    if (saving && !transaction.write(content, disk.revision, disk, result.message, result.status))
        return result;
    if (apply) {
        Apply(content, recheck.toBool(false), result);
        m_activeContent = content;
        m_activeRevision = disk.revision;
        qInfo().noquote() << "clientversions.cfg applied:" << m_rules->RuleCount() << "rule(s),"
                          << result.cooldownsCleared << "cooldown(s) cleared," << result.rechecked
                          << "online session(s) rechecked," << result.wouldDeny << "denied";
    }
    finish();
    return result;
}
