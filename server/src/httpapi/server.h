// httpapi::Server — batch REST API, health/ready/metrics, /v1/capabilities,
// the streaming WS route, and the static frontend. Direct port of
// app/internal/httpapi/server.go + handlers.go, on top of uWS::App.
#pragma once

#include <App.h>
#include <atomic>
#include <map>
#include <string>

#include "../config.h"
#include "../obs/log.h"
#include "../obs/metrics.h"
#include "../pipeline/pipeline.h"

namespace sb::httpapi {

struct Deps {
  const Config *config = nullptr;
  pipeline::Pipeline *pipeline = nullptr;
  const obs::Logger *logger = nullptr;
  obs::Metrics *metrics = nullptr;
  std::map<std::string, std::string> core_state;  // core name -> "ok" | error
  std::string web_dist_dir;                       // built Vue frontend (may be empty)
};

// A simple counting semaphore for batch-endpoint queue admission (mirrors
// server.go's `sem chan struct{}`).
class QueueGate {
 public:
  explicit QueueGate(int capacity) : capacity_(capacity < 1 ? 1 : capacity) {}
  bool TryAcquire() {
    int cur = in_flight_.load();
    while (cur < capacity_) {
      if (in_flight_.compare_exchange_weak(cur, cur + 1)) return true;
    }
    return false;
  }
  void Release() { in_flight_.fetch_sub(1); }
  int InFlight() const { return in_flight_.load(); }

 private:
  std::atomic<int> in_flight_{0};
  int capacity_;
};

class Server {
 public:
  explicit Server(Deps deps);

  // Runs the event loop. Blocks until the process is signaled to stop (see
  // main.cpp) — uWS has no built-in graceful-shutdown hook, so main.cpp
  // simply lets the OS reclaim the process on SIGINT/SIGTERM after in-flight
  // work drains via QueueGate reaching zero (best-effort; see docs/blockers.md).
  void Run();

  bool CheckAuth(std::string_view authHeader) const;
  bool CheckAuthWS(std::string_view authHeader, std::string_view accessTokenQuery) const;

 private:
  void RegisterRoutes();

  Deps deps_;
  uWS::App app_;
  QueueGate queue_;
  std::atomic<int64_t> active_streams_{0};
};

}  // namespace sb::httpapi
