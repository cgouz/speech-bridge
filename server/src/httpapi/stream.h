// stream — the /v1/stream WebSocket endpoint. Adapts uWS WebSocket frames to
// a Session (session.h): one connection = one speaker -> one target
// language. Direct port of app/internal/stream/stream.go.
#pragma once

#include <App.h>
#include <atomic>

#include "server.h"

namespace sb::httpapi {

void RegisterStream(uWS::App &app, Deps &deps, Server &server, std::atomic<int64_t> &activeStreams);

}  // namespace sb::httpapi
