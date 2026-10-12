// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_npauth.h"

#include <optional>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrlQuery>
#include "client_session.h"
#include "database.h"
#include "npauth.h"
#include "webapi_auth.h"

namespace WebApiRoutes {
namespace {

using Status = QHttpServerResponse::StatusCode;

const QString kTokenExchange = "urn:ietf:params:oauth:grant-type:token-exchange";
const QString kAccessTokenType = "urn:ietf:params:oauth:token-type:access_token";
const QString kIdTokenType = "urn:ietf:params:oauth:token-type:id_token";

QHttpServerResponse Reply(const QJsonObject& body, Status status = Status::Ok) {
    QHttpServerResponse response("application/json",
                                 QJsonDocument(body).toJson(QJsonDocument::Compact), status);
    auto headers = response.headers();
    headers.append("Cache-Control", "no-store");
    headers.append("Pragma", "no-cache");
    response.setHeaders(headers);
    return response;
}

QHttpServerResponse PublicReply(const QJsonObject& body) {
    auto response = Reply(body);
    auto headers = response.headers();
    headers.replaceOrAppend("Cache-Control", "public, max-age=300");
    headers.removeAll("Pragma");
    response.setHeaders(headers);
    return response;
}

Status StatusFor(const QString& error) {
    if (error == "invalid_client")
        return Status::Unauthorized;
    if (error == "server_error")
        return Status::InternalServerError;
    if (error == "temporarily_unavailable")
        return Status::ServiceUnavailable;
    return Status::BadRequest;
}

QHttpServerResponse Error(const NpAuthService::Result& failure, bool basicAuth = false) {
    QJsonObject body{{"error", failure.error}, {"error_description", failure.description}};
    if (failure.code)
        body.insert("error_code", failure.code);
    auto response = Reply(body, StatusFor(failure.error));
    if (basicAuth && failure.error == "invalid_client") {
        auto headers = response.headers();
        headers.append("WWW-Authenticate", "Basic realm=\"shadNet\"");
        response.setHeaders(headers);
    }
    return response;
}

const auto kUnavailable = NpAuthService::Failure(
    "temporarily_unavailable", "Service dependency is temporarily unavailable", 9001);
const auto kMissing =
    NpAuthService::Failure("invalid_request", "Mandatory parameter is missing", 4098);
const auto kMalformed = NpAuthService::Failure("invalid_request", "Parameter is malformed", 4099);

// Form parameters, each at most once (RFC 6749 section 3.2).
std::optional<QHash<QString, QString>> ParseForm(const QByteArray& body) {
    QHash<QString, QString> params;
    const QUrlQuery query(QString::fromUtf8(body).replace('+', ' '));
    for (const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if (key.isEmpty() || params.contains(key) || value.contains(QChar::Null))
            return std::nullopt;
        params.insert(key, value);
    }
    return params;
}

// client_secret_basic: form-encoded client_id:client_secret (RFC 6749 section 2.3.1).
bool ParseBasic(const QByteArray& header, QString& id, QByteArray& secret) {
    if (!header.startsWith("Basic "))
        return false;
    const auto decoded =
        QByteArray::fromBase64(header.mid(6).trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    const auto colon = decoded.indexOf(':');
    if (colon <= 0)
        return false;
    auto field = [](QByteArray value) {
        return QUrl::fromPercentEncoding(value.replace('+', ' '));
    };
    id = field(decoded.left(colon));
    secret = field(decoded.mid(colon + 1)).toUtf8();
    return true;
}

} // namespace

void RegisterNpAuthRoutes(QHttpServer& http, Database& db, SharedState& shared,
                          NpAuthService& issuer) {
    http.route("/.well-known/jwks.json", QHttpServerRequest::Method::Get,
               [&issuer](const QHttpServerRequest&) {
                   if (!issuer.IsEnabled())
                       return Error(kUnavailable);
                   return PublicReply(issuer.PublicKeys());
               });
    http.route("/.well-known/oauth-authorization-server", QHttpServerRequest::Method::Get,
               [&issuer](const QHttpServerRequest&) {
                   if (!issuer.IsEnabled())
                       return Error(kUnavailable);
                   auto base = issuer.Issuer();
                   while (base.endsWith('/'))
                       base.chop(1);
                   return PublicReply(
                       QJsonObject{{"issuer", issuer.Issuer()},
                                   {"token_endpoint", base + "/2.0/oauth/token"},
                                   {"jwks_uri", base + "/.well-known/jwks.json"},
                                   {"grant_types_supported", QJsonArray{kTokenExchange}},
                                   {"token_endpoint_auth_methods_supported",
                                    QJsonArray{"client_secret_post", "client_secret_basic"}}});
               });
    http.route(
        "/2.0/oauth/token", QHttpServerRequest::Method::Post,
        [&db, &shared, &issuer](const QHttpServerRequest& req) {
            if (!issuer.IsEnabled())
                return Error(kUnavailable);
            if (req.url().hasQuery())
                return Error(NpAuthService::Failure("invalid_request",
                                                    "Query parameters not allowed for POST", 4130));
            if (req.body().size() > 8192 ||
                req.value("Content-Type").split(';').first().trimmed().toLower() !=
                    "application/x-www-form-urlencoded")
                return Error(kMalformed);
            const auto form = ParseForm(req.body());
            if (!form)
                return Error(kMalformed);
            const auto& params = *form;

            NpAuthService::Request request;
            const auto authorization = req.value("Authorization");
            const bool basic = !authorization.isEmpty();
            if (basic) {
                if (authorization.size() > 4096 || params.contains("client_secret") ||
                    !ParseBasic(authorization, request.clientId, request.clientSecret))
                    return Error(kMalformed);
            } else {
                request.clientId = params.value("client_id");
                request.clientSecret = params.value("client_secret").toUtf8();
            }
            for (const auto* name :
                 {"grant_type", "subject_token", "subject_token_type", "scope"}) {
                if (params.value(name).isEmpty())
                    return Error(kMissing);
            }
            if (params.value("grant_type") != kTokenExchange)
                return Error(NpAuthService::Failure("unsupported_grant_type",
                                                    "Unsupported grant type", 4152));
            if (params.value("subject_token_type") != kAccessTokenType ||
                params.value("requested_token_type", kIdTokenType) != kIdTokenType)
                return Error(kMalformed);
            const auto version = params.value("version");
            if (!version.isEmpty() && version != "3")
                return Error(kMalformed);
            request.version = version.isEmpty() ? "legacy" : "v3";
            request.scope = params.value("scope");

            const auto auth = WebApiAuth::AuthenticateToken(params.value("subject_token"), db);
            const auto account = auth.userId ? db.GetUserRow(*auth.userId) : std::nullopt;
            if (!account || account->banned)
                return Error(NpAuthService::Failure("invalid_grant", "Subject token is not valid"));
            QString title;
            {
                QReadLocker lock(&shared.clientsLock);
                const auto client = shared.clients.constFind(*auth.userId);
                if (client == shared.clients.constEnd())
                    return Error(
                        NpAuthService::Failure("invalid_grant", "Account has no online session"));
                title = client->npTitleId;
            }
            const auto result = issuer.Issue(std::move(request), *auth.userId, auth.npid, title);
            if (!result.error.isEmpty())
                return Error(result, basic);
            return Reply(QJsonObject{{"access_token", QString::fromLatin1(result.token)},
                                     {"issued_token_type", kIdTokenType},
                                     {"token_type", "N_A"},
                                     {"expires_in", 120},
                                     {"scope", result.scope}});
        });
}
} // namespace WebApiRoutes
