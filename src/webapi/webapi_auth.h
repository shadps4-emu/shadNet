// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QString>

#include "database.h"

namespace WebApiAuth {

struct AuthResult {
    std::optional<qint64> userId;
    QString npid; // canonical npid of the authed account, when authenticated
    QHttpServerResponse errorResponse{QHttpServerResponse::StatusCode::Unauthorized};
};

// Extract `Authorization: Bearer <token>` from the request and look up
// the matching `account` row. Returns the userId on success.
//
// On failure the AuthResult.userId is nullopt and errorResponse is
// pre-built with a JSON body shaped like real PSN's auth errors:
//   { "error": { "code": <int>, "message": "<string>" } }
//
// Codes (mirror of common PSN WebAPI auth failures):
//   - missing/malformed Authorization header = 401, code 0x80920002
//   - token doesn't match any account         = 401, code 0x80920001
AuthResult Authenticate(const QHttpServerRequest& req, Database& db);

} // namespace WebApiAuth
