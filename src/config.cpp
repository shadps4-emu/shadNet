// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QFile>
#include <QRandomGenerator>
#include <QSettings>
#include <QTextStream>
#include "config.h"

void ConfigManager::LoadBannedDomains() {
    m_bannedDomains.clear();
    QFile f("domains_banlist.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString d = in.readLine().trimmed().toLower();
        if (!d.isEmpty() && !d.startsWith('#'))
            m_bannedDomains.insert(d);
    }
    qInfo() << "Loaded" << m_bannedDomains.size() << "banned domains";
}

QString ConfigManager::EnsureMemberApiKey() {
    QWriteLocker lk(&m_lock);
    if (!m_memberApiKey.isEmpty())
        return QString();

    static const QString chars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                "abcdefghijklmnopqrstuvwxyz"
                                                "0123456789");
    QString key;
    key.reserve(48);
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < 48; ++i)
        key.append(chars.at(gen->bounded(chars.size())));

    QSettings s(m_path, QSettings::IniFormat);
    s.setValue(QStringLiteral("MemberApiKey"), key);
    s.sync();
    if (s.status() != QSettings::NoError) {
        qCritical() << "Could not write a generated MemberApiKey to" << m_path;
        return QString();
    }

    m_memberApiKey = key;
    return key;
}

QString ConfigManager::EnsureAdminApiKey() {
    QWriteLocker lk(&m_lock);
    if (!m_adminApiKey.isEmpty())
        return QString();

    static const QString chars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                "abcdefghijklmnopqrstuvwxyz"
                                                "0123456789");
    QString key;
    key.reserve(48);
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < 48; ++i)
        key.append(chars.at(gen->bounded(chars.size())));

    QSettings s(m_path, QSettings::IniFormat);
    s.setValue(QStringLiteral("AdminApiKey"), key);
    s.sync();
    if (s.status() != QSettings::NoError) {
        qCritical() << "Could not write a generated AdminApiKey to" << m_path;
        return QString();
    }

    m_adminApiKey = key;
    return key;
}

void ConfigManager::Parse(const QString& path) {
    QWriteLocker lk(&m_lock);

    m_path = path;

    QSettings s(path, QSettings::IniFormat);

    auto str = [&](const QString& key, const QString& def) -> QString {
        if (!s.contains(key))
            s.setValue(key, def);
        return s.value(key, def).toString();
    };
    auto boolean = [&](const QString& key, bool def) -> bool {
        if (!s.contains(key))
            s.setValue(key, def);
        return s.value(key, def).toBool();
    };
    auto strList = [&](const QString& key) -> QStringList {
        if (!s.contains(key))
            s.setValue(key, QString());
        QString raw = s.value(key).toString();
        return raw.isEmpty() ? QStringList{} : raw.split(',', Qt::SkipEmptyParts);
    };

    m_host = str("Host", "127.0.0.1");
    m_unsecured_port = str("UnsecuredPort", "31313");
    m_matchingUdpPort = str("MatchingUdpPort", "31314");
    m_webapiPort = str("WebApiPort", "31315");
    m_statsEnabled = boolean("StatsEnabled", true);
    m_matching2Enabled = boolean("Matching2Enabled", false);
    m_trophiesEnabled = boolean("TrophiesEnabled", true);
    m_memberApiEnabled = boolean("MemberApiEnabled", false);
    m_memberApiHost = str("MemberApiHost", "127.0.0.1");
    m_memberApiPort = str("MemberApiPort", "31360");
    m_memberApiKey = str("MemberApiKey", "");
    m_statsPort = str("StatsPort", "31320");
    m_statsPath = str("StatsPath", "stats");
    m_statsCacheLife = str("StatsCacheLife", "30").toInt();
    m_emailValidated = boolean("EmailValidated", false);
    m_adminApiEnabled = boolean("AdminApiEnabled", true);
    m_adminApiHost = str("AdminApiHost", "127.0.0.1");
    m_adminApiPort = str("AdminApiPort", "31350");
    m_adminSessionMinutes = str("AdminSessionMinutes", "480").toInt();
    m_adminApiKey = str("AdminApiKey", "");
    // Accounts named here are granted admin in the database at startup. This is the
    // only way to create the first admin on a fresh install.
    m_adminsList = strList("AdminsList");
    m_registrationSecretKey = str("RegistrationSecretKey", "");

    s.sync();

    if (s.status() != QSettings::NoError)
        qWarning() << "QSettings error reading" << path;
    else
        qInfo() << "Config loaded from" << path;

    if (!m_registrationSecretKey.isEmpty())
        qInfo() << "Registration requires secret key";
    else
        qInfo() << "Registration is open (no secret key set)";

    LoadBannedDomains();
}

bool ConfigManager::Load(const QString& path) {
    Parse(path);
    return true;
}

void ConfigManager::Reload(const QString& path) {
    Parse(path.isEmpty() ? m_path : path);
    qInfo() << "Config reloaded";
}
