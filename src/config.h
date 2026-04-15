// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QReadLocker>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWriteLocker>

class ConfigManager {
public:
    bool Load(const QString& path = "shadnet.cfg");
    void Reload(const QString& path = "shadnet.cfg");

    QString GetHost() const {
        QReadLocker lk(&m_lock);
        return m_host;
    }
    QString GetUnsecuredPort() const {
        QReadLocker lk(&m_lock);
        return m_unsecured_port;
    }
    bool IsEmailValidated() const {
        QReadLocker lk(&m_lock);
        return m_emailValidated;
    }
    bool IsBannedDomain(const QString& d) const {
        QReadLocker lk(&m_lock);
        return m_bannedDomains.contains(d.toLower());
    }
    bool IsAdmin(const QString& npid) const {
        QReadLocker lk(&m_lock);
        return m_adminsList.contains(npid);
    }

    // Returns true if registration is allowed for the given secret_key.
    // When RegistrationSecretKey is empty, all registrations are allowed.
    // When set, only requests carrying the matching key are accepted.
    bool IsRegistrationAllowed(const QString& key) const {
        QReadLocker lk(&m_lock);
        return m_registrationSecretKey.isEmpty() ||
               (!key.isEmpty() && key == m_registrationSecretKey);
    }

    void SetHost(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_host = v;
    }
    void SetUnsecuredPort(const QString& v) {
        QWriteLocker lk(&m_lock);
        m_unsecured_port = v;
    }
    void SetEmailValidated(bool v) {
        QWriteLocker lk(&m_lock);
        m_emailValidated = v;
    }
    void SetAdminsList(const QStringList& v) {
        QWriteLocker lk(&m_lock);
        m_adminsList = v;
    }
    void LoadBannedDomains();

private:
    void Parse(const QString& path);

    mutable QReadWriteLock m_lock;
    QString m_path;

    // config values
    QString m_host = "0.0.0.0";
    QString m_unsecured_port = "31313";
    bool m_emailValidated = false;
    QStringList m_adminsList;
    QSet<QString> m_bannedDomains;
    // When non-empty, registrations must supply this key or they are rejected.
    QString m_registrationSecretKey;
};
