// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;
class NpAuthService;
struct SharedState;

namespace WebApiRoutes {
void RegisterNpAuthRoutes(QHttpServer& http, Database& db, SharedState& shared,
                          NpAuthService& auth);
}
