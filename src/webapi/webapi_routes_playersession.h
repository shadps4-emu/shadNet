// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

class QHttpServer;
class Database;
struct SharedState;

namespace WebApiRoutes {

// Session Manager "player sessions" (the NpWebApi2 sessionManager API group): create, join,
// leave, invite, update and message, plus the friends' sessions listing.
void RegisterPlayerSessionRoutes(QHttpServer& http, Database& db, SharedState& shared);

// Remove a user from every player session they are in. Called from the client disconnect path:
// an offline user auto-leaves their sessions, and the remaining members are notified.
void PurgeUserFromPlayerSessions(SharedState& shared, int64_t userId);

} // namespace WebApiRoutes
