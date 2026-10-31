// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QHttpServer;
class Database;
struct SharedState;

namespace WebApiRoutes {

void RegisterGameCustomDataRoutes(QHttpServer& http, Database& db, SharedState& shared);

} // namespace WebApiRoutes
