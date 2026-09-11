#include "server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "errors.h"
#include "handlers.h"
#include "stream.h"

namespace fs = std::filesystem;

namespace sb::httpapi {

namespace {

std::pair<std::string, int> SplitHostPort(const std::string &bind) {
  size_t colon = bind.rfind(':');
  return {bind.substr(0, colon), std::stoi(bind.substr(colon + 1))};
}

const std::map<std::string, std::string> &MimeTypes() {
  static const std::map<std::string, std::string> m = {
      {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},    {".json", "application/json"},
      {".svg", "image/svg+xml"},              {".png", "image/png"},
      {".ico", "image/x-icon"},               {".woff2", "font/woff2"},
      {".map", "application/json"},
  };
  return m;
}

std::string RouteLabel(const std::string &path) {
  if (path == "/health" || path == "/ready" || path == "/metrics") return path;
  if (path.rfind("/v1/", 0) == 0) return path;
  return "/";
}

}  // namespace

Server::Server(Deps deps) : deps_(std::move(deps)), queue_(deps_.config->queue_depth) { RegisterRoutes(); }

bool Server::CheckAuth(std::string_view authHeader) const {
  const std::string &token = deps_.config->auth_token;
  if (token.empty()) return true;
  constexpr std::string_view kPrefix = "Bearer ";
  std::string_view got = authHeader.substr(0, kPrefix.size()) == kPrefix ? authHeader.substr(kPrefix.size()) : "";
  return got == token;
}

bool Server::CheckAuthWS(std::string_view authHeader, std::string_view accessTokenQuery) const {
  const std::string &token = deps_.config->auth_token;
  if (token.empty()) return true;
  constexpr std::string_view kPrefix = "Bearer ";
  std::string_view got = authHeader.substr(0, kPrefix.size()) == kPrefix ? authHeader.substr(kPrefix.size()) : "";
  if (got == token) return true;
  return accessTokenQuery == token;
}

void Server::RegisterRoutes() {
  RegisterHandlers(app_, deps_, *this, queue_);
  RegisterStream(app_, deps_, *this, active_streams_);

  // Static frontend (SPA) — registered last; uWS's router still prefers the
  // more specific patterns registered above for exact matches.
  if (!deps_.web_dist_dir.empty()) {
    app_.get("/*", [this](auto *res, auto *req) {
      std::string path(req->getUrl());
      fs::path root = fs::path(deps_.web_dist_dir);
      fs::path rel = path.empty() || path == "/" ? "index.html" : path.substr(1);
      fs::path full = (root / rel).lexically_normal();
      // Refuse to escape web_dist_dir (defends against a path like /../..).
      auto rootStr = root.lexically_normal().string();
      if (full.string().compare(0, rootStr.size(), rootStr) != 0) full = root / "index.html";
      if (!fs::is_regular_file(full)) full = root / "index.html";

      std::ifstream f(full, std::ios::binary);
      if (!f) {
        res->writeStatus(kStatus500)->end("frontend not found");
        return;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      std::string body = ss.str();

      auto ext = full.extension().string();
      auto it = MimeTypes().find(ext);
      std::string mime = it != MimeTypes().end() ? it->second : "application/octet-stream";
      res->writeHeader("Content-Type", mime)->end(body);
    });
  }
}

void Server::Run() {
  auto [host, port] = SplitHostPort(deps_.config->bind);
  app_.listen(host, port, [this, host = host, port = port](auto *listenSocket) {
    if (listenSocket) {
      deps_.logger->Info("http listening", {{"addr", deps_.config->bind}});
    } else {
      deps_.logger->Error("failed to listen", {{"addr", deps_.config->bind}});
    }
  });
  app_.run();
}

}  // namespace sb::httpapi
