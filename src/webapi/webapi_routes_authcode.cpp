// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_authcode.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUuid>

#include "client_session.h"
#include "database.h"
#include "npauth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

constexpr int MaxFormBodyLength = 8192;

constexpr int ERR_BAD_CLIENT_CREDENTIALS = 4102;
constexpr int ERR_MISSING_PARAMETER = 4098;
constexpr int ERR_MALFORMED_PARAMETER = 4099;
constexpr int ERR_QUERY_ON_POST = 4130;
constexpr int ERR_UNAUTHORIZED_CLIENT = 4151;
constexpr int ERR_UNSUPPORTED_GRANT_TYPE = 4152;
constexpr int ERR_CLIENT_NOT_FOUND = 4161;
constexpr int ERR_ACCOUNT_UNAUTHORIZED = 3174913;

constexpr char ConsoleRedirectUri[] = "orbis://games";

QHttpServerResponse NoStore(QHttpServerResponse response) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::CacheControl, "no-store");
    response.setHeaders(std::move(headers));
#else
    response.setHeader("Cache-Control", "no-store");
#endif
    return response;
}

QHttpServerResponse OAuthError(QHttpServerResponse::StatusCode status, const QString& error,
                               const QString& description, int code = 0) {
    QJsonObject body;
    body.insert(QStringLiteral("error"), error);
    body.insert(QStringLiteral("error_description"), description);
    if (code != 0) {
        body.insert(QStringLiteral("error_code"), code);
    }
    return NoStore(QHttpServerResponse{"application/json",
                                       QJsonDocument(body).toJson(QJsonDocument::Compact), status});
}

QHttpServerResponse OAuthOk(const QJsonObject& body) {
    return NoStore(QHttpServerResponse{"application/json",
                                       QJsonDocument(body).toJson(QJsonDocument::Compact),
                                       QHttpServerResponse::StatusCode::Ok});
}

// application/x-www-form-urlencoded body. Returns nullopt when a parameter repeats.
std::optional<QHash<QString, QString>> ParseForm(const QByteArray& body) {
    QHash<QString, QString> out;
    for (const QByteArray& pair : body.split('&')) {
        if (pair.isEmpty()) {
            continue;
        }
        const qsizetype eq = pair.indexOf('=');
        QByteArray rawKey = eq < 0 ? pair : pair.left(eq);
        QByteArray rawValue = eq < 0 ? QByteArray() : pair.mid(eq + 1);
        rawKey.replace('+', ' ');
        rawValue.replace('+', ' ');
        const QString key = QUrl::fromPercentEncoding(rawKey);
        if (out.contains(key)) {
            return std::nullopt;
        }
        out.insert(key, QUrl::fromPercentEncoding(rawValue));
    }
    return out;
}

struct ClientCredentials {
    QString id;
    QString secret;
};

// HTTP Basic credentials, or client_id/client_secret in the form body.
std::optional<ClientCredentials> ReadClient(const QHttpServerRequest& req,
                                            const QHash<QString, QString>* form) {
    const QByteArray auth = req.value("Authorization").trimmed();
    if (auth.startsWith("Basic ") || auth.startsWith("basic ")) {
        const QByteArray decoded = QByteArray::fromBase64(auth.mid(6).trimmed());
        const qsizetype colon = decoded.indexOf(':');
        if (colon <= 0) {
            return std::nullopt;
        }
        return ClientCredentials{QString::fromUtf8(decoded.left(colon)),
                                 QString::fromUtf8(decoded.mid(colon + 1))};
    }
    if (form && !form->value(QStringLiteral("client_id")).isEmpty()) {
        return ClientCredentials{form->value(QStringLiteral("client_id")),
                                 form->value(QStringLiteral("client_secret"))};
    }
    return std::nullopt;
}

std::optional<QHttpServerResponse> RejectBadSecret(const SharedState& shared,
                                                   const ClientCredentials& client) {
    if (!shared.npAuth || shared.npAuth->CheckClient(client.id, client.secret.toUtf8()) !=
                              NpAuthService::ClientCheck::BadSecret) {
        return std::nullopt;
    }
    auto response =
        OAuthError(QHttpServerResponse::StatusCode::Unauthorized, QStringLiteral("invalid_client"),
                   QStringLiteral("Bad client credentials"), ERR_BAD_CLIENT_CREDENTIALS);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.append(QHttpHeaders::WellKnownHeader::WWWAuthenticate, "Basic realm=\"shadNet\"");
    response.setHeaders(std::move(headers));
#else
    response.setHeader("WWW-Authenticate", "Basic realm=\"shadNet\"");
#endif
    return response;
}

QHttpServerResponse MissingParameter(const QString& name) {
    return OAuthError(
        QHttpServerResponse::StatusCode::BadRequest, QStringLiteral("invalid_request"),
        QStringLiteral("Mandatory parameter '%1' is missing").arg(name), ERR_MISSING_PARAMETER);
}

QHttpServerResponse InvalidClient() {
    return OAuthError(QHttpServerResponse::StatusCode::Unauthorized,
                      QStringLiteral("invalid_client"),
                      QStringLiteral("Client credentials are missing"), ERR_CLIENT_NOT_FOUND);
}

QString StableUuid(const char* kind, int64_t userId) {
    static const QUuid ns =
        QUuid::fromString(QStringLiteral("6f9c3a52-6d1b-4a8e-9f0d-5b2c7e41a3d8"));
    return QUuid::createUuidV5(ns, QStringLiteral("%1:%2").arg(QLatin1String(kind)).arg(userId))
        .toString(QUuid::WithoutBraces);
}

QHttpServerResponse AccountError(QHttpServerResponse::StatusCode status, int code,
                                 const QString& message) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject body;
    body.insert(QStringLiteral("error"), error);
    return NoStore(QHttpServerResponse{"application/json",
                                       QJsonDocument(body).toJson(QJsonDocument::Compact), status});
}

} // namespace

void RegisterAuthCodeRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // GET /.well-known/jwks.json
    // Public key a game backend checks NPAuth ID token signatures with.
    http.route("/.well-known/jwks.json", QHttpServerRequest::Method::Get,
               [&shared](const QHttpServerRequest&) -> QHttpServerResponse {
                   if (!shared.npAuth || !shared.npAuth->IsEnabled()) {
                       return OAuthError(
                           QHttpServerResponse::StatusCode::ServiceUnavailable,
                           QStringLiteral("temporarily_unavailable"),
                           QStringLiteral("Service dependency is temporarily unavailable"), 9001);
                   }
                   QHttpServerResponse response{
                       "application/json",
                       QJsonDocument(shared.npAuth->PublicKeys()).toJson(QJsonDocument::Compact),
                       QHttpServerResponse::StatusCode::Ok};
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                   QHttpHeaders headers = response.headers();
                   headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::CacheControl,
                                           "public, max-age=300");
                   response.setHeaders(std::move(headers));
#else
                   response.setHeader("Cache-Control", "public, max-age=300");
#endif
                   return response;
               });

    // POST /2.0/oauth/token   (application/x-www-form-urlencoded, client credentials as Basic)
    //   grant_type=authorization_code&code=<code>&redirect_uri=orbis%3A%2F%2Fgames
    http.route(
        "/2.0/oauth/token", QHttpServerRequest::Method::Post,
        [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!req.url().query().isEmpty()) {
                return OAuthError(
                    QHttpServerResponse::StatusCode::BadRequest, QStringLiteral("invalid_request"),
                    QStringLiteral("Query parameters not allowed for POST"), ERR_QUERY_ON_POST);
            }
            const QByteArray body = req.body();
            const auto form = body.size() <= MaxFormBodyLength ? ParseForm(body) : std::nullopt;
            if (!form) {
                return OAuthError(
                    QHttpServerResponse::StatusCode::BadRequest, QStringLiteral("invalid_request"),
                    QStringLiteral("Parameter is malformed"), ERR_MALFORMED_PARAMETER);
            }
            const QString grantType = form->value(QStringLiteral("grant_type"));
            if (grantType.isEmpty()) {
                return MissingParameter(QStringLiteral("grant_type"));
            }
            if (grantType == QLatin1String("client_credentials")) {
                return OAuthError(
                    QHttpServerResponse::StatusCode::BadRequest,
                    QStringLiteral("unauthorized_client"),
                    QStringLiteral("Client does not have grant type: %1").arg(grantType),
                    ERR_UNAUTHORIZED_CLIENT);
            }
            if (grantType != QLatin1String("authorization_code")) {
                return OAuthError(QHttpServerResponse::StatusCode::BadRequest,
                                  QStringLiteral("unsupported_grant_type"),
                                  QStringLiteral("Unsupported grant type: %1").arg(grantType),
                                  ERR_UNSUPPORTED_GRANT_TYPE);
            }
            const auto client = ReadClient(req, &*form);
            if (!client) {
                return InvalidClient();
            }
            if (auto rejected = RejectBadSecret(shared, *client)) {
                return std::move(*rejected);
            }
            const QString code = form->value(QStringLiteral("code"));
            if (code.isEmpty()) {
                return MissingParameter(QStringLiteral("code"));
            }
            const QString redirectUri = form->value(QStringLiteral("redirect_uri"));
            if (redirectUri.isEmpty()) {
                return MissingParameter(QStringLiteral("redirect_uri"));
            }
            if (redirectUri != QLatin1String(ConsoleRedirectUri)) {
                return OAuthError(QHttpServerResponse::StatusCode::BadRequest,
                                  QStringLiteral("redirect_uri_mismatch"),
                                  QStringLiteral("Redirect URI mismatch"));
            }

            const auto grant = shared.authCodes.Redeem(code);
            if (!grant || grant->clientId != client->id || !db.GetUsername(grant->userId)) {
                return OAuthError(QHttpServerResponse::StatusCode::BadRequest,
                                  QStringLiteral("invalid_grant"), QStringLiteral("Bad Request"));
            }
            const QString accessToken = shared.authCodes.IssueAccessToken(*grant);
            qInfo().noquote() << "WebAPI: authorization code redeemed for account" << grant->userId
                              << "client" << grant->clientId << "from"
                              << req.remoteAddress().toString();

            QJsonObject out;
            out.insert(QStringLiteral("access_token"), accessToken);
            out.insert(QStringLiteral("token_type"), QStringLiteral("bearer"));
            out.insert(QStringLiteral("expires_in"), AuthCodeStore::AccessTokenLifetimeSecs);
            out.insert(QStringLiteral("scope"), grant->scope);
            return OAuthOk(out);
        });

    // GET /2.0/oauth/token/<access token>   (client credentials as Basic)
    // Validates an access token and tells the backend which account it belongs to.
    http.route(
        "/2.0/oauth/token/<arg>", QHttpServerRequest::Method::Get,
        [&db, &shared](const QString& token, const QHttpServerRequest& req) -> QHttpServerResponse {
            const auto client = ReadClient(req, nullptr);
            if (!client) {
                return InvalidClient();
            }
            if (auto rejected = RejectBadSecret(shared, *client)) {
                return std::move(*rejected);
            }
            const auto grant = shared.authCodes.FindAccessToken(token);
            if (!grant) {
                return OAuthError(QHttpServerResponse::StatusCode::BadRequest,
                                  QStringLiteral("invalid_token"),
                                  QStringLiteral("Token not found"));
            }
            const auto onlineId = db.GetUsername(grant->userId);
            if (grant->clientId != client->id || !onlineId) {
                return OAuthError(QHttpServerResponse::StatusCode::BadRequest,
                                  QStringLiteral("invalid_token"),
                                  QStringLiteral("Token is invalid"));
            }

            QJsonObject out;
            out.insert(QStringLiteral("scopes"), grant->scope);
            out.insert(QStringLiteral("expiration"), QDateTime::fromSecsSinceEpoch(grant->expiresAt)
                                                         .toUTC()
                                                         .toString(Qt::ISODateWithMs));
            out.insert(QStringLiteral("user_id"), QString::number(grant->userId));
            out.insert(QStringLiteral("client_id"), grant->clientId);
            out.insert(QStringLiteral("duid"), QString());
            out.insert(QStringLiteral("device_type"), QStringLiteral("PS4"));
            out.insert(QStringLiteral("is_sub_account"), false);
            out.insert(QStringLiteral("online_id"), *onlineId);
            out.insert(QStringLiteral("country_code"),
                       QString::fromLatin1(DefaultRegion).toUpper());
            out.insert(QStringLiteral("language_code"),
                       QString::fromLatin1(AccountDefaults::Language));
            out.insert(QStringLiteral("user_uuid"), StableUuid("user", grant->userId));
            out.insert(QStringLiteral("dcim_id"), StableUuid("dcim", grant->userId));
            return OAuthOk(out);
        });

    // GET /vl/api/v1/s2s/users/me/info   (Authorization: Bearer <access token>)
    // Account information for the access token's account.
    http.route(
        "/vl/api/v1/s2s/users/me/info", QHttpServerRequest::Method::Get,
        [&db, &shared](const QHttpServerRequest& req) -> QHttpServerResponse {
            const QByteArray auth = req.value("Authorization").trimmed();
            const QString token = auth.toLower().startsWith("bearer ")
                                      ? QString::fromUtf8(auth.mid(7).trimmed())
                                      : QString();
            const auto grant =
                token.isEmpty() ? std::nullopt : shared.authCodes.FindAccessToken(token);
            const auto onlineId = grant ? db.GetUsername(grant->userId) : std::nullopt;
            if (!onlineId) {
                return AccountError(QHttpServerResponse::StatusCode::Unauthorized,
                                    ERR_ACCOUNT_UNAUTHORIZED, QStringLiteral("Unauthorized"));
            }

            QJsonObject parental;
            // all these are dummy values, todo if we need to implement them
            parental.insert(QStringLiteral("RestrictChat"), false);
            parental.insert(QStringLiteral("RestrictUGM"), false);
            parental.insert(QStringLiteral("RestrictStoreContent"), false);
            QJsonObject out;
            out.insert(QStringLiteral("accountId"), QString::number(grant->userId));
            out.insert(QStringLiteral("age"), AccountDefaults::Age());
            out.insert(QStringLiteral("communityDomain"),
                       QString::fromLatin1(AccountDefaults::CommunityDomain));
            out.insert(QStringLiteral("dateOfBirth"),
                       AccountDefaults::DateOfBirth().toString(QStringLiteral("yyyy/MM/dd")));
            out.insert(QStringLiteral("language"), QString::fromLatin1(AccountDefaults::Language));
            out.insert(QStringLiteral("onlineId"), *onlineId);
            out.insert(QStringLiteral("parentalControl"), parental);
            out.insert(QStringLiteral("region"), QString::fromLatin1(DefaultRegion));
            out.insert(QStringLiteral("subaccount"), false);
            return NoStore(QHttpServerResponse{"application/json",
                                               QJsonDocument(out).toJson(QJsonDocument::Compact),
                                               QHttpServerResponse::StatusCode::Created});
        });
}

} // namespace WebApiRoutes
