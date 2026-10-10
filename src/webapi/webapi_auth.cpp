// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_auth.h"

#include <QByteArray>
#include <QDebug>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

namespace {

QHttpServerResponse JsonErrorReply(QHttpServerResponse::StatusCode status, quint32 code,
                                   const QString& message) {
    QJsonObject errorObj;
    errorObj.insert("code", static_cast<qint64>(code));
    errorObj.insert("message", message);
    QJsonObject body;
    body.insert("error", errorObj);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact), status};
}

// Sony error codes for WebAPI auth failures. Picked to mirror what real
// PSN returns when an OAuth bearer is missing/invalid.
constexpr quint32 ERR_INVALID_TOKEN = 0x80920001;  // bearer doesn't resolve to a user
constexpr quint32 ERR_INVALID_HEADER = 0x80920002; // missing/malformed Authorization header

} // namespace

namespace WebApiAuth {

AuthResult Authenticate(const QHttpServerRequest& req, Database& db) {
    AuthResult out;

    const QByteArray rawAuth = req.value("Authorization");
    if (rawAuth.isEmpty()) {
        out.errorResponse = JsonErrorReply(QHttpServerResponse::StatusCode::Unauthorized,
                                           ERR_INVALID_HEADER, "Missing Authorization header");
        return out;
    }

    const QString authStr = QString::fromUtf8(rawAuth).trimmed();
    static const QString bearerPrefix = QStringLiteral("Bearer ");
    if (!authStr.startsWith(bearerPrefix, Qt::CaseInsensitive)) {
        out.errorResponse =
            JsonErrorReply(QHttpServerResponse::StatusCode::Unauthorized, ERR_INVALID_HEADER,
                           "Authorization scheme must be Bearer");
        return out;
    }
    const QString token = authStr.mid(bearerPrefix.size()).trimmed();
    if (token.isEmpty()) {
        out.errorResponse = JsonErrorReply(QHttpServerResponse::StatusCode::Unauthorized,
                                           ERR_INVALID_HEADER, "Empty bearer token");
        return out;
    }

    QSqlQuery q(db.Conn());
    q.prepare(QStringLiteral("SELECT user_id, username FROM account WHERE token = ?"));
    q.addBindValue(token);
    if (!q.exec()) {
        qWarning() << "WebApiAuth: account lookup failed:" << q.lastError().text();
        out.errorResponse = JsonErrorReply(QHttpServerResponse::StatusCode::InternalServerError,
                                           ERR_INVALID_TOKEN, "Auth lookup failed");
        return out;
    }
    if (!q.next()) {
        // No account has this token. Don't leak whether the token was
        // ever valid (e.g. revoked); just say "invalid token" uniformly.
        out.errorResponse = JsonErrorReply(QHttpServerResponse::StatusCode::Unauthorized,
                                           ERR_INVALID_TOKEN, "Invalid bearer token");
        return out;
    }

    out.userId = q.value(0).toLongLong();
    out.npid = q.value(1).toString();
    return out;
}

} // namespace WebApiAuth
