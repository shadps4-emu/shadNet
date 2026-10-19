// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;

namespace WebApiRoutes {

// Store/commerce container lookups (GET /v5/container). shadNet has no catalogue, so every
// requested container is reported as unavailable.
void RegisterCommerceRoutes(QHttpServer& http, Database& db);

} // namespace WebApiRoutes
