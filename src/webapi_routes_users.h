// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;

namespace WebApiRoutes {

// Routes registered:
//   GET /v1/users/<arg>/friendList -mutual friends (or other buckets via friendStatus=)
//   GET /v1/users/<arg>/blockList  -users the caller has blocked
void RegisterUserRoutes(QHttpServer& http, Database& db);

} // namespace WebApiRoutes
