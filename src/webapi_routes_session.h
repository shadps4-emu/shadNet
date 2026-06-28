// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;
struct SharedState;

namespace WebApiRoutes {

// PSN Session Manager Web API. Forwarded generically from the emulator (sceNpWebApi ->
// shadNet), so only server-side routes are required here.
void RegisterSessionRoutes(QHttpServer& http, Database& db, SharedState& shared);

} // namespace WebApiRoutes
