// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;

namespace WebApiRoutes {

void RegisterUserRoutes(QHttpServer& http, Database& db);

} // namespace WebApiRoutes