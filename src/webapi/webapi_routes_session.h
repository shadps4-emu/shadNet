// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

class QHttpServer;
class Database;
struct SharedState;

namespace WebApiRoutes {

void RegisterSessionRoutes(QHttpServer& http, Database& db, SharedState& shared);

// Remove a user from every session they are in (owner-migration / owner-bind teardown).
// Called from the client disconnect path: an offline user auto-leaves their sessions.
void PurgeUserFromSessions(SharedState& shared, int64_t userId);

} // namespace WebApiRoutes
