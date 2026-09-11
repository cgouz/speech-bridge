// handlers — REST route registration: health/ready/metrics/capabilities and
// the guarded batch endpoints. Direct port of app/internal/httpapi/handlers.go.
#pragma once

#include <App.h>

#include "server.h"

namespace sb::httpapi {

void RegisterHandlers(uWS::App &app, Deps &deps, Server &server, QueueGate &queue);

}  // namespace sb::httpapi
