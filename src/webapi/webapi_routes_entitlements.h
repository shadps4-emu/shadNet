// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

class QHttpServer;
class QString;
class Database;
struct SharedState;

namespace WebApiRoutes {

void RegisterEntitlementRoutes(QHttpServer& http, Database& db, SharedState& shared);
QString CallerTitleId(SharedState& shared, int64_t userId);

} // namespace WebApiRoutes
