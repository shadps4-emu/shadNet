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
    QString GetMatchingUdpPort() const {
        QReadLocker lk(&m_lock);
        return m_matchingUdpPort;
    }
    QString GetWebApiPort() const {
        QReadLocker lk(&m_lock);
        return m_webapiPort;
    }
    bool IsStatsEnabled() const {
        QReadLocker lk(&m_lock);
        return m_statsEnabled;
    }
    QString GetStatsPort() const {
        QReadLocker lk(&m_lock);
        return m_statsPort;
    }
    QString GetStatsPath() const {
        QReadLocker lk(&m_lock);
        return m_statsPath;
    }
    int GetStatsCacheLife() const {
        QReadLocker lk(&m_lock);
        return m_statsCacheLife;
    }
    bool IsMatching2Enabled() const {
        QReadLocker lk(&m_lock);
        return m_matching2Enabled;
    }
    bool IsTrophiesEnabled() const {
        QReadLocker lk(&m_lock);
        return m_trophiesEnabled;
    }
    bool IsAdminApiEnabled() const {
        QReadLocker lk(&m_lock);
        return m_adminApiEnabled;
    }
    QString GetAdminApiHost() const {
        QReadLocker lk(&m_lock);
        return m_adminApiHost;
    }
    QString GetAdminApiPort() const {
        QReadLocker lk(&m_lock);
        return m_adminApiPort;
    }
    int GetAdminSessionMinutes() const {
        QReadLocker lk(&m_lock);
        return m_adminSessionMinutes;
    }
    QString GetAdminApiKey() const {
        QReadLocker lk(&m_lock);
        return m_adminApiKey;
    }
    bool IsAdminApiKeyRequired() const {
        QReadLocker lk(&m_lock);
        return !m_adminApiKey.isEmpty();
    }
    QString EnsureAdminApiKey();
    QStringList GetAdminsList() const {
        QReadLocker lk(&m_lock);
        return m_adminsList;
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
    bool IsRegistrationKeyRequired() const {
        QReadLocker lk(&m_lock);
        return !m_registrationSecretKey.isEmpty();
    }

    bool IsMemberApiEnabled() const {
        QReadLocker lk(&m_lock);
        return m_memberApiEnabled;
    }
    QString GetMemberApiHost() const {
        QReadLocker lk(&m_lock);
        return m_memberApiHost;
    }
    QString GetMemberApiPort() const {
        QReadLocker lk(&m_lock);
        return m_memberApiPort;
    }

    QString GetMemberApiKey() const {
        QReadLocker lk(&m_lock);
        return m_memberApiKey;
    }
    bool IsMemberApiKeyRequired() const {
        QReadLocker lk(&m_lock);
        return !m_memberApiKey.isEmpty();
    }

    QString EnsureMemberApiKey();

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
    QString m_matchingUdpPort = "31314";
    QString m_webapiPort = "31315";
    bool m_statsEnabled = true;
    bool m_matching2Enabled = false;
    bool m_trophiesEnabled = true;
    QString m_statsPort = "31320";
    QString m_statsPath = "stats";
    int m_statsCacheLife = 30; // seconds the stats JSON is cached before recompute
    bool m_emailValidated = false;
    bool m_adminApiEnabled = true;
    QString m_adminApiHost = "0.0.0.0";
    QString m_adminApiPort = "31350";
    int m_adminSessionMinutes = 480;
    QString m_adminApiKey;
    QStringList m_adminsList;
    QSet<QString> m_bannedDomains;
    QString m_registrationSecretKey;
    bool m_memberApiEnabled = true;
    QString m_memberApiHost = "0.0.0.0";
    QString m_memberApiPort = "31360";
    QString m_memberApiKey;
};
