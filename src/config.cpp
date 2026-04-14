// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QFile>
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
    m_emailValidated = boolean("EmailValidated", false);
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
