// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

typedef struct evp_pkey_st EVP_PKEY;

// NPAuth ID tokens (sceNpAuthGetIdTokenA/V3) and the client registry they rely on.
class NpAuthService {
public:
    // OAuth error plus the auth server's numeric code (0 when there is none).
    struct Error {
        QString error;
        QString description;
        int code = 0;
    };

    struct Request {
        QString clientId;
        QByteArray clientSecret;
        QString scope;
    };

    struct Account {
        qint64 accountId = 0;
        QString onlineId;
        QString titleId;
    };

    struct Result {
        QByteArray token; // empty on failure
        int expiresIn = 0;
        Error error;
    };

    enum class ClientCheck { Unregistered, Ok, BadSecret };

    static constexpr int MaxClientIdLength = 128;
    static constexpr int MaxClientSecretLength = 256;
    static constexpr int MaxScopeLength = 1024;
    // Issuer ID of the retail NP environment, carried as a string.
    static constexpr char EnvironmentIssuerId[] = "256";
    bool Load(const QString& path, QString& error);
    bool IsEnabled() const;
    QJsonObject PublicKeys() const;
    ClientCheck CheckClient(const QString& clientId, const QByteArray& secret) const;

    Result Issue(Request request, const Account& account) const;

private:
    struct Client {
        QByteArray secretHash;
        QStringList titles; // empty: any title
        QStringList scopes; // empty: every supported scope
    };

    QHash<QString, Client> m_clients;
    std::shared_ptr<EVP_PKEY> m_key;
    QString m_issuer;
    QString m_keyId;
    int m_lifetime = 0;
    QJsonObject m_publicKeys;
};
