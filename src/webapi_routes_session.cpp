// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_session.h"

#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include "client_session.h" // SharedState
#include "database.h"
#include "webapi_auth.h"
#include "webapi_routes_common.h"

namespace WebApiRoutes {

namespace {

// (Session helpers / builders go here.)

} // namespace

void RegisterSessionRoutes(QHttpServer& http, Database& db, SharedState& shared) {
    // PSN Session Manager Web API routes are registered here, e.g.:
    //   http.route("/v1/sessions", QHttpServerRequest::Method::Post, [&db, &shared](...) { ... });
    //
    // Sessions are stateful (members, owner, session data, lifetime), so a session store will
    // live in SharedState under its own lock -- mirror the matchmaking Room model in
    // cmd_matching.cpp. Each route authenticates via WebApiAuth::Authenticate and replies with
    // JsonOk / JsonError from webapi_routes_common.h.
    (void)http;
    (void)db;
    (void)shared;
}

} // namespace WebApiRoutes
