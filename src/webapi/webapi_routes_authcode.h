// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;
struct SharedState;

namespace WebApiRoutes {

// Auth endpoints a game backend uses to turn an authorization code into the player account:
// POST /2.0/oauth/token, GET /2.0/oauth/token/<token> and GET /vl/api/v1/s2s/users/me/info.
void RegisterAuthCodeRoutes(QHttpServer& http, Database& db, SharedState& shared);

} // namespace WebApiRoutes
