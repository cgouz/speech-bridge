// sb-server — one process that dlopens the four core libraries and runs the
// streaming + batch speech-to-speech pipeline. C++ port of
// app/cmd/sb-server/main.go: replaces the Go orchestrator entirely; the four
// core libs and their vendored engines are untouched.
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "config.h"
#include "httpapi/server.h"
#include "native/core.h"
#include "obs/log.h"
#include "obs/metrics.h"
#include "pipeline/pipeline.h"

namespace fs = std::filesystem;

namespace {
constexpr const char *kVersion = "0.1.0";  // TODO: set via -DSB_VERSION at build time
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-version") == 0 || std::strcmp(argv[i], "--version") == 0) {
      std::printf("sb-server %s\n", kVersion);
      return 0;
    }
  }

  sb::Config cfg;
  try {
    cfg = sb::LoadConfig();
  } catch (const sb::ConfigError &e) {
    std::fprintf(stderr, "sb-server: %s\n", e.what());
    return 2;
  }

  sb::obs::Logger log(cfg.log_format, cfg.log_level);
  sb::obs::Metrics metrics;

  log.Info("sb-server starting", {{"version", kVersion},
                                   {"bind", cfg.bind},
                                   {"lib_dir", cfg.lib_dir},
                                   {"models_dir", cfg.models_dir}});

  sb::core::Options opt;
  opt.lib_dir = cfg.lib_dir;
  opt.device = cfg.device;
  opt.stt_model = cfg.stt_model;
  opt.mt_model = cfg.mt_model;
  opt.mt_ctx = cfg.mt_ctx;
  opt.magpie_model = cfg.magpie_model;
  opt.vits_dir = cfg.vits_dir;

  sb::core::Set coreSet = sb::core::Load(opt);
  for (auto &[name, st] : coreSet.report) {
    if (st == "ok") {
      log.Info("core loaded", {{"core", name}});
    } else {
      log.Warn("core not available", {{"core", name}, {"reason", st}});
    }
  }

  sb::pipeline::Engines engines;
  engines.stt = coreSet.stt.get();
  engines.mt = coreSet.mt.get();
  engines.magpie = coreSet.tts_magpie.get();
  engines.vits = coreSet.tts_vits.get();

  std::unique_ptr<sb::pipeline::Pipeline> pipe;
  try {
    pipe = std::make_unique<sb::pipeline::Pipeline>(engines);
  } catch (const std::exception &e) {
    log.Error("pipeline init failed", {{"err", e.what()}});
    return 1;
  }

  sb::httpapi::Deps deps;
  deps.config = &cfg;
  deps.pipeline = pipe.get();
  deps.logger = &log;
  deps.metrics = &metrics;
  deps.core_state = coreSet.report;

  // Frontend: web/dist next to the binary, or SB_WEB_DIST_DIR override.
  const char *webDistEnv = std::getenv("SB_WEB_DIST_DIR");
  fs::path webDist = webDistEnv && *webDistEnv ? fs::path(webDistEnv) : fs::path("web/dist");
  if (fs::is_directory(webDist)) {
    deps.web_dist_dir = webDist.string();
  } else {
    log.Warn("web frontend not found; / will 500", {{"path", webDist.string()}});
  }

  sb::httpapi::Server server(std::move(deps));

  log.Info("ready", {});
  server.Run();  // blocks; see docs/blockers.md for the graceful-shutdown gap
  log.Info("shutdown complete", {});
  return 0;
}
