// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_users.h"

#include <optional>

#include <QDebug>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QUrlQuery>

#include "database.h"
#include "webapi_auth.h"

namespace WebApiRoutes {

void RegisterUserRoutes(QHttpServer& http, Database& db) {}

} // namespace WebApiRoutes
