// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "npauth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
#include <openssl/core_names.h>
#endif

#include "account_defaults.h"

namespace {

constexpr int DefaultLifetimeSecs = 3600;
constexpr int MinLifetimeSecs = 60;
constexpr int MaxLifetimeSecs = 86400;
constexpr int MaxTokenLength = 4096; // OrbisNpIdToken holds 4096 characters plus a terminator

// Each scope adds the claims named after it. Plain "openid" adds the claims OpenID Connect
// requires of every ID token. "openid:duid" is not in the public scope list, a game asks for
// it and reads a 64-character device ID.
const QSet<QString> SupportedScopes = {
    "openid",           "openid:acct_id",   "openid:acct_id_str",
    "openid:age",       "openid:aud",       "openid:comm_domain",
    "openid:ctry_code", "openid:dob",       "openid:env_iss_id",
    "openid:exp",       "openid:iat",       "openid:iss",
    "openid:lang",      "openid:online_id", "openid:duid",
};

QByteArray Base64Url(const QByteArray& bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString EncodeNumber(const BIGNUM* number) {
    QByteArray bytes(BN_num_bytes(number), Qt::Uninitialized);
    BN_bn2bin(number, reinterpret_cast<unsigned char*>(bytes.data()));
    return QString::fromLatin1(Base64Url(bytes));
}

QJsonObject PublicJwk(EVP_PKEY* key) {
    QJsonObject result;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
    BIGNUM* modulus = nullptr;
    BIGNUM* exponent = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &modulus) == 1 &&
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &exponent) == 1) {
        result.insert(QStringLiteral("n"), EncodeNumber(modulus));
        result.insert(QStringLiteral("e"), EncodeNumber(exponent));
    }
    BN_free(modulus);
    BN_free(exponent);
#else
    const RSA* rsa = EVP_PKEY_get0_RSA(key);
    if (rsa) {
        const BIGNUM* modulus = nullptr;
        const BIGNUM* exponent = nullptr;
        RSA_get0_key(rsa, &modulus, &exponent, nullptr);
        result.insert(QStringLiteral("n"), EncodeNumber(modulus));
        result.insert(QStringLiteral("e"), EncodeNumber(exponent));
    }
#endif
    return result;
}

bool ReadList(const QJsonValue& value, QStringList& output, int maxLength) {
    if (value.isUndefined())
        return true;
    if (!value.isArray() || value.toArray().size() > 64)
        return false;
    for (const auto& item : value.toArray()) {
        if (!item.isString())
            return false;
        const auto text = item.toString();
        if (text.isEmpty() || text.size() > maxLength || text != text.trimmed() ||
            output.contains(text))
            return false;
        output.append(text);
    }
    return true;
}

bool HasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    return true;
}

NpAuthService::Result Fail(const QString& error, const QString& description, int code) {
    NpAuthService::Result result;
    result.error = {error, description, code};
    return result;
}

} // namespace

bool NpAuthService::Load(const QString& path, QString& error) {
    m_key.reset();
    m_clients.clear();
    m_publicKeys = {};
    if (path.isEmpty())
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    if (file.size() > 1024 * 1024) {
        error = QStringLiteral("%1 is larger than 1 MiB").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("%1 is not a JSON object: %2").arg(path, parseError.errorString());
        return false;
    }
    const auto config = document.object();
    if (!HasOnlyKeys(config, {"issuer", "signing_key", "token_lifetime", "clients"}) ||
        !config.value("issuer").isString() || !config.value("signing_key").isString() ||
        !config.value("clients").isArray()) {
        error = QStringLiteral("expected issuer, signing_key, clients and optional token_lifetime");
        return false;
    }

    const QString issuer = config.value("issuer").toString();
    const QUrl issuerUrl(issuer, QUrl::StrictMode);
    if (!issuerUrl.isValid() || issuerUrl.scheme() != "https" || issuerUrl.host().isEmpty() ||
        !issuerUrl.userInfo().isEmpty() || issuerUrl.hasQuery() || issuerUrl.hasFragment() ||
        issuer.size() > 256) {
        error = QStringLiteral("issuer must be an https URL without query or fragment");
        return false;
    }

    int lifetime = DefaultLifetimeSecs;
    if (config.contains("token_lifetime")) {
        const auto value = config.value("token_lifetime");
        lifetime = value.isDouble() ? value.toInt(-1) : -1;
        if (lifetime < MinLifetimeSecs || lifetime > MaxLifetimeSecs ||
            value.toDouble() != lifetime) {
            error = QStringLiteral("token_lifetime must be whole seconds between %1 and %2")
                        .arg(MinLifetimeSecs)
                        .arg(MaxLifetimeSecs);
            return false;
        }
    }

    const auto clients = config.value("clients").toArray();
    if (clients.size() > 1024) {
        error = QStringLiteral("more than 1024 clients");
        return false;
    }
    QHash<QString, Client> registered;
    const QRegularExpression hashPattern(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    for (const auto& value : clients) {
        const auto entry = value.toObject();
        const auto id = entry.value("client_id").toString();
        const auto hash = entry.value("client_secret_sha256").toString();
        Client client;
        if (!value.isObject() ||
            !HasOnlyKeys(entry, {"client_id", "client_secret_sha256", "titles", "scopes"}) ||
            id.isEmpty() || id.toUtf8().size() > MaxClientIdLength || registered.contains(id) ||
            !hashPattern.match(hash).hasMatch() ||
            !ReadList(entry.value("titles"), client.titles, 16) ||
            !ReadList(entry.value("scopes"), client.scopes, 64)) {
            error = QStringLiteral("invalid client entry %1").arg(id.isEmpty() ? "(no id)" : id);
            return false;
        }
        for (const auto& scope : std::as_const(client.scopes)) {
            if (!SupportedScopes.contains(scope)) {
                error = QStringLiteral("client %1 lists unsupported scope %2").arg(id, scope);
                return false;
            }
        }
        client.secretHash = QByteArray::fromHex(hash.toLatin1());
        registered.insert(id, client);
    }

    const auto keyName = config.value("signing_key").toString();
    QFile keyFile(QFileInfo(path).dir().filePath(keyName));
    if (keyName.isEmpty() || !keyFile.open(QIODevice::ReadOnly) || keyFile.size() > 65536) {
        error = QStringLiteral("cannot read signing key %1").arg(keyName);
        return false;
    }
    auto pem = keyFile.readAll();
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(
        BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
    std::shared_ptr<EVP_PKEY> key(
        bio ? PEM_read_bio_PrivateKey(
                  bio.get(), nullptr, [](char*, int, int, void*) { return 0; }, nullptr)
            : nullptr,
        EVP_PKEY_free);
    OPENSSL_cleanse(pem.data(), static_cast<size_t>(pem.size()));
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA || EVP_PKEY_bits(key.get()) < 2048 ||
        EVP_PKEY_bits(key.get()) > 4096) {
        error = QStringLiteral("signing key must be an unencrypted 2048-4096 bit RSA PEM");
        return false;
    }
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
    const auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
        EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    const bool keyOk = context && EVP_PKEY_private_check(context.get()) == 1;
#else
    const bool keyOk = RSA_check_key(EVP_PKEY_get0_RSA(key.get())) == 1;
#endif
    auto jwk = PublicJwk(key.get());
    if (!keyOk || jwk.size() != 2) {
        error = QStringLiteral("signing key failed its consistency check");
        return false;
    }

    jwk.insert("kty", "RSA");
    m_keyId = QString::fromLatin1(Base64Url(QCryptographicHash::hash(
        QJsonDocument(jwk).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)));
    jwk.insert("kid", m_keyId);
    jwk.insert("alg", "RS256");
    jwk.insert("use", "sig");
    m_publicKeys.insert("keys", QJsonArray{jwk});
    m_issuer = issuer;
    m_lifetime = lifetime;
    m_clients = std::move(registered);
    m_key = std::move(key);
    return true;
}

bool NpAuthService::IsEnabled() const {
    return static_cast<bool>(m_key);
}

QJsonObject NpAuthService::PublicKeys() const {
    return m_publicKeys;
}

NpAuthService::ClientCheck NpAuthService::CheckClient(const QString& clientId,
                                                      const QByteArray& secret) const {
    const auto client = m_clients.constFind(clientId);
    if (client == m_clients.constEnd())
        return ClientCheck::Unregistered;
    const auto hash = QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
    return CRYPTO_memcmp(client->secretHash.constData(), hash.constData(), 32) == 0
               ? ClientCheck::Ok
               : ClientCheck::BadSecret;
}

NpAuthService::Result NpAuthService::Issue(Request request, const Account& account) const {
    if (!m_key)
        return Fail("temporarily_unavailable", "Service dependency is temporarily unavailable",
                    9001);
    if (request.clientId.isEmpty())
        return Fail("invalid_request", "Mandatory parameter 'client_id' is missing", 4098);
    if (request.clientSecret.isEmpty())
        return Fail("invalid_request", "Mandatory parameter 'client_secret' is missing", 4098);
    if (request.scope.trimmed().isEmpty())
        return Fail("invalid_request", "Mandatory parameter 'scope' is missing", 4098);
    if (request.clientId.toUtf8().size() > MaxClientIdLength)
        return Fail("invalid_request", "Parameter 'client_id' is malformed", 4099);
    if (request.clientSecret.size() > MaxClientSecretLength)
        return Fail("invalid_request", "Parameter 'client_secret' is malformed", 4099);
    if (request.scope.toUtf8().size() > MaxScopeLength)
        return Fail("invalid_request", "Parameter 'scope' is malformed", 4099);

    const auto check = CheckClient(request.clientId, request.clientSecret);
    OPENSSL_cleanse(request.clientSecret.data(), static_cast<size_t>(request.clientSecret.size()));
    if (check == ClientCheck::Unregistered)
        return Fail("invalid_client", "The client was not found", 4161);
    if (check == ClientCheck::BadSecret)
        return Fail("invalid_client", "Bad client credentials", 4102);
    const Client& client = *m_clients.constFind(request.clientId);
    if (account.accountId <= 0 || account.onlineId.isEmpty())
        return Fail("server_error", "An unknown error has occurred", 2);
    if (!client.titles.isEmpty() && !client.titles.contains(account.titleId))
        return Fail("invalid_client", "Invalid client", 4173);

    auto scopes =
        request.scope.split(QRegularExpression(QStringLiteral("[ ,]+")), Qt::SkipEmptyParts);
    scopes.removeDuplicates();
    for (const auto& scope : std::as_const(scopes)) {
        if (!SupportedScopes.contains(scope) ||
            (!client.scopes.isEmpty() && !client.scopes.contains(scope)))
            return Fail("invalid_scope", "Invalid scope", 4153);
    }
    const auto has = [&scopes](const char* scope) { return scopes.contains(QLatin1String(scope)); };
    const bool core = has("openid");

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonObject claims;
    if (core || has("openid:iss"))
        claims.insert("iss", m_issuer);
    if (core || has("openid:aud"))
        claims.insert("aud", request.clientId);
    if (core || has("openid:iat"))
        claims.insert("iat", now);
    if (core || has("openid:exp"))
        claims.insert("exp", now + m_lifetime);
    if (has("openid:env_iss_id"))
        claims.insert("env_iss_id", QString::fromLatin1(EnvironmentIssuerId));
    if (has("openid:acct_id"))
        claims.insert("acct_id", account.accountId);
    if (has("openid:acct_id_str"))
        claims.insert("acct_id_str", QString::number(account.accountId));
    if (has("openid:online_id"))
        claims.insert("online_id", account.onlineId);
    if (has("openid:ctry_code"))
        claims.insert("ctry_code", QString::fromLatin1(AccountDefaults::Region).toUpper());
    if (has("openid:lang"))
        claims.insert("lang", QString::fromLatin1(AccountDefaults::Language));
    if (has("openid:comm_domain"))
        claims.insert("comm_domain", QString::fromLatin1(AccountDefaults::CommunityDomain));
    if (has("openid:dob"))
        claims.insert("dob", AccountDefaults::DateOfBirth().toString(Qt::ISODate));
    if (has("openid:age"))
        claims.insert("age", AccountDefaults::Age());
    if (has("openid:duid")) {
        // Stable per account and issuer; not a real console ID.
        const auto seed =
            QStringLiteral("shadnet-duid\n%1\n%2").arg(m_issuer).arg(account.accountId);
        claims.insert(
            "duid",
            QString::fromLatin1(
                QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).toHex()));
    }

    const QJsonObject header{{"alg", "RS256"}, {"typ", "JWT"}, {"kid", m_keyId}};
    QByteArray token = Base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.' +
                       Base64Url(QJsonDocument(claims).toJson(QJsonDocument::Compact));

    const auto context =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* keyContext = nullptr;
    size_t length = 0;
    if (!context ||
        EVP_DigestSignInit(context.get(), &keyContext, EVP_sha256(), nullptr, m_key.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(keyContext, RSA_PKCS1_PADDING) <= 0 ||
        EVP_DigestSignUpdate(context.get(), token.constData(), token.size()) != 1 ||
        EVP_DigestSignFinal(context.get(), nullptr, &length) != 1 || length > 512)
        return Fail("server_error", "An unknown error has occurred", 2);
    QByteArray signature(static_cast<qsizetype>(length), Qt::Uninitialized);
    if (EVP_DigestSignFinal(context.get(), reinterpret_cast<unsigned char*>(signature.data()),
                            &length) != 1)
        return Fail("server_error", "An unknown error has occurred", 2);
    signature.resize(static_cast<qsizetype>(length));
    token += '.' + Base64Url(signature);
    if (token.size() > MaxTokenLength)
        return Fail("server_error", "An unknown error has occurred", 2);

    Result result;
    result.token = token;
    result.expiresIn = m_lifetime;
    return result;
}
