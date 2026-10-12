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
#include <openssl/rand.h>
#include <openssl/rsa.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
#include <openssl/core_names.h>
#endif

namespace {

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
    if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 32)
        return false;
    for (const auto& item : value.toArray()) {
        if (!item.isString())
            return false;
        const auto text = item.toString();
        if (text.isEmpty() || text.size() > maxLength || text.contains(QChar::Null) ||
            text != text.trimmed() || output.contains(text))
            return false;
        output.append(text);
    }
    return true;
}

bool HasOnlyKeys(const QJsonObject& object, const QSet<QString>& keys) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key()))
            return false;
    }
    return object.size() == keys.size();
}

} // namespace

bool NpAuthService::Load(const QString& path) {
    m_key.reset();
    m_applications.clear();
    m_publicKeys = {};
    if (path.isEmpty())
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto config = document.object();
    if (!HasOnlyKeys(config, {"issuer", "signing_key", "applications"}) ||
        !config.value("issuer").isString() || !config.value("signing_key").isString() ||
        !config.value("applications").isArray())
        return false;
    const QUrl issuer(config.value("issuer").toString(), QUrl::StrictMode);
    if (!issuer.isValid() || issuer.scheme() != "https" || issuer.host().isEmpty() ||
        !issuer.userInfo().isEmpty() || issuer.hasQuery() || issuer.hasFragment() ||
        issuer.toString().size() > 256)
        return false;

    const auto applications = config.value("applications").toArray();
    if (applications.isEmpty() || applications.size() > 1024)
        return false;
    QHash<QString, Application> registered;
    const QRegularExpression hashPattern(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    const QRegularExpression scopePattern(QStringLiteral("\\A[A-Za-z0-9:_./-]{1,64}\\z"));
    for (const auto& value : applications) {
        if (!value.isObject())
            return false;
        const auto app = value.toObject();
        if (!HasOnlyKeys(app,
                         {"client_id", "client_secret_sha256", "titles", "scopes", "versions"}) ||
            !app.value("client_id").isString() || !app.value("client_secret_sha256").isString())
            return false;
        const auto id = app.value("client_id").toString();
        const auto hash = app.value("client_secret_sha256").toString();
        Application entry;
        if (id.isEmpty() || id.toUtf8().size() > 128 || id.contains(QChar::Null) ||
            registered.contains(id) || !hashPattern.match(hash).hasMatch() ||
            !ReadList(app.value("titles"), entry.titles, 64) ||
            !ReadList(app.value("scopes"), entry.scopes, 64) ||
            !ReadList(app.value("versions"), entry.versions, 6) || !entry.scopes.contains("openid"))
            return false;
        for (const auto& scope : entry.scopes) {
            if (!scopePattern.match(scope).hasMatch())
                return false;
        }
        for (const auto& version : entry.versions) {
            if (version != "legacy" && version != "v3")
                return false;
        }
        entry.secretHash = QByteArray::fromHex(hash.toLatin1());
        registered.insert(id, entry);
    }

    const auto keyName = config.value("signing_key").toString();
    if (keyName.isEmpty())
        return false;
    QFile keyFile(QFileInfo(path).dir().filePath(keyName));
    if (!keyFile.open(QIODevice::ReadOnly) || keyFile.size() > 65536)
        return false;
    auto pem = keyFile.readAll();
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(
        BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
    if (!bio)
        return false;
    std::shared_ptr<EVP_PKEY> key(
        PEM_read_bio_PrivateKey(
            bio.get(), nullptr, [](char*, int, int, void*) { return 0; }, nullptr),
        EVP_PKEY_free);
    OPENSSL_cleanse(pem.data(), static_cast<size_t>(pem.size()));
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA || EVP_PKEY_bits(key.get()) < 2048 ||
        EVP_PKEY_bits(key.get()) > 4096)
        return false;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
    const auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
        EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_private_check(context.get()) != 1)
        return false;
#else
    if (RSA_check_key(EVP_PKEY_get0_RSA(key.get())) != 1)
        return false;
#endif
    auto jwk = PublicJwk(key.get());
    if (jwk.size() != 2)
        return false;
    jwk.insert("kty", "RSA");
    const auto canonical = QJsonDocument(jwk).toJson(QJsonDocument::Compact);
    m_keyId = QString::fromLatin1(
        Base64Url(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)));
    jwk.insert("kid", m_keyId);
    jwk.insert("alg", "RS256");
    jwk.insert("use", "sig");
    m_publicKeys.insert("keys", QJsonArray{jwk});
    m_issuer = config.value("issuer").toString();
    m_applications = std::move(registered);
    m_key = std::move(key);
    return true;
}

bool NpAuthService::IsEnabled() const {
    return static_cast<bool>(m_key);
}

QJsonObject NpAuthService::PublicKeys() const {
    return m_publicKeys;
}

QString NpAuthService::Issuer() const {
    return m_issuer;
}

NpAuthService::Result NpAuthService::Failure(const QString& error, const QString& description,
                                             int code) {
    return {{}, error, description, code};
}

NpAuthService::Result NpAuthService::Issue(Request request, qint64 accountId,
                                           const QString& onlineId, const QString& titleId) const {
    if (!m_key)
        return Failure("temporarily_unavailable", "Service dependency is temporarily unavailable",
                       9001);
    const auto& id = request.clientId;
    if (id.isEmpty() || request.clientSecret.isEmpty() || request.scope.isEmpty())
        return Failure("invalid_request", "Mandatory parameter is missing", 4098);
    if (id.toUtf8().size() > 128 || request.clientSecret.size() > 256 ||
        request.scope.toUtf8().size() > 1024)
        return Failure("invalid_request", "Parameter is malformed", 4099);
    const auto app = m_applications.constFind(id);
    const auto secretHash =
        QCryptographicHash::hash(request.clientSecret, QCryptographicHash::Sha256);
    OPENSSL_cleanse(request.clientSecret.data(), static_cast<size_t>(request.clientSecret.size()));
    if (app == m_applications.constEnd())
        return Failure("invalid_client", "The client was not found", 4161);
    if (CRYPTO_memcmp(app->secretHash.constData(), secretHash.constData(), 32) != 0)
        return Failure("invalid_client", "Bad client credentials", 4102);
    if (!app->versions.contains(request.version))
        return Failure("unauthorized_client", "Client is not registered for this version");
    if (accountId <= 0 || onlineId.isEmpty() || !app->titles.contains(titleId))
        return Failure("unauthorized_client", "Client is not registered for this title");
    // OAuth separates scopes with spaces; some games pass a comma list.
    auto scopes =
        request.scope.split(QRegularExpression(QStringLiteral("[ ,]+")), Qt::SkipEmptyParts);
    scopes.removeDuplicates();
    if (!scopes.contains("openid"))
        return Failure("invalid_scope", "Invalid scope", 4153);
    for (const auto& requested : scopes) {
        if (!app->scopes.contains(requested))
            return Failure("invalid_scope", "Invalid scope", 4153);
    }
    QByteArray nonce(16, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1)
        return Failure("server_error", "An unknown error has occurred", 2);
    const auto now = QDateTime::currentSecsSinceEpoch();
    QJsonObject claims{{"iss", m_issuer},
                       {"sub", QString::number(accountId)},
                       {"aud", id},
                       {"iat", now},
                       {"exp", now + 120},
                       {"jti", QString::fromLatin1(Base64Url(nonce))},
                       {"env_iss_id", kEnvironmentIssuerId},
                       {"online_id", onlineId},
                       {"np_title_id", titleId},
                       {"scope", scopes.join(' ')}};
    if (scopes.contains("openid:duid")) {
        const auto seed =
            (QStringLiteral("shadnet-duid\n") + m_issuer + '\n' + QString::number(accountId))
                .toUtf8();
        claims.insert("duid",
                      QString::fromLatin1(
                          QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex()));
    }
    const QJsonObject header{{"alg", "RS256"}, {"typ", "JWT"}, {"kid", m_keyId}};
    auto token = Base64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.' +
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
        return Failure("server_error", "An unknown error has occurred", 2);
    QByteArray signature(static_cast<qsizetype>(length), Qt::Uninitialized);
    if (EVP_DigestSignFinal(context.get(), reinterpret_cast<unsigned char*>(signature.data()),
                            &length) != 1)
        return Failure("server_error", "An unknown error has occurred", 2);
    signature.resize(static_cast<qsizetype>(length));
    token += '.' + Base64Url(signature);
    if (token.size() > 4096)
        return Failure("server_error", "An unknown error has occurred", 2);
    return {token, {}, {}, 0, scopes.join(' ')};
}
