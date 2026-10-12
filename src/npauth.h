// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

typedef struct evp_pkey_st EVP_PKEY;

class NpAuthService {
public:
    struct Result {
        QByteArray token;
        QString error;
        QString description;
        int code = 0;
        QString scope; // granted scopes on success
    };

    struct Request {
        QString clientId;
        QByteArray clientSecret;
        QString scope;
        QString version; // "legacy" (GetIdTokenA) or "v3"
    };

    static Result Failure(const QString& error, const QString& description, int code = 0);

    bool Load(const QString& path);
    bool IsEnabled() const;
    QJsonObject PublicKeys() const;
    QString Issuer() const;
    Result Issue(Request request, qint64 accountId, const QString& onlineId,
                 const QString& titleId) const;

    static constexpr int kEnvironmentIssuerId = 256;

private:
    struct Application {
        QByteArray secretHash;
        QStringList titles;
        QStringList scopes;
        QStringList versions;
    };

    QHash<QString, Application> m_applications;
    std::shared_ptr<EVP_PKEY> m_key;
    QString m_issuer;
    QString m_keyId;
    QJsonObject m_publicKeys;
};
