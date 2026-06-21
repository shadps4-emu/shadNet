// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;

namespace WebApiRoutes {

// Routes registered:
//   GET /v1/users/<arg>/friendList -mutual friends (or other buckets via friendStatus=)
//   GET /v1/users/<arg>/blockList  -users the caller has blocked
//   GET   /v1/users/<arg>/container/<arg>          -np_commerce2 product container
//   GET   /v1/users/<arg>/entitlements             -owned entitlements (all titles)
//   GET   /v1/users/<arg>/entitlements/<arg>       -single entitlement lookup
//   PATCH /v1/users/<arg>/entitlements/<arg>       -consume one use
void RegisterUserRoutes(QHttpServer& http, Database& db);

} // namespace WebApiRoutes
