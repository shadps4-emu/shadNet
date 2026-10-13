// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "client_version_rules.h"

#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>

QString ClientVersionRules::DefaultFileContent() {
    return defaultClientVersionsContent();
}

bool ClientVersionRules::Load(const QString& path, QStringList& errors) {
    {
        QWriteLocker lk(&m_lock);
        m_rules.clear();
    }
    QFile file(path);
    if (!file.exists()) {
        QSaveFile out(path);
        const QByteArray bytes = DefaultFileContent().toUtf8();
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
            errors.append(QStringLiteral("Cannot create %1: %2").arg(path, out.errorString()));
            return false;
        }
    }
    if (!file.open(QIODevice::ReadOnly)) {
        errors.append(QStringLiteral("Cannot read %1: %2").arg(path, file.errorString()));
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(file.readAll());
    if (decoder.hasError()) {
        errors.append(QStringLiteral("%1 must be UTF-8 text.").arg(path));
        return false;
    }
    QStringList lineErrors;
    Apply(text, lineErrors);
    for (const QString& e : lineErrors)
        errors.append(QStringLiteral("%1 %2").arg(path, e));
    return true;
}

void ClientVersionRules::Apply(const QString& content, QStringList& errors) {
    const ClientVersionsConfig parsed = parseClientVersionsConfig(content);
    for (const auto& e : parsed.errors)
        errors.append(QStringLiteral("line %1: %2").arg(e.line).arg(e.message));
    QWriteLocker lk(&m_lock);
    m_rules = parsed.rules;
}

ClientVersionRules::Decision ClientVersionRules::Evaluate(const QString& version) const {
    QReadLocker lk(&m_lock);
    const ClientVersionDecision d = evaluateClientVersion(m_rules, version);
    return {d.allowed, d.message, d.line};
}

int ClientVersionRules::RuleCount() const {
    QReadLocker lk(&m_lock);
    return static_cast<int>(m_rules.size());
}
