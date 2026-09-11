// errors — the stable error shape (see docs/api.md) plus small JSON response
// helpers for uWS's HttpResponse<false> (this project never uses SSL — see
// third_party/PINNED.md). Direct port of app/internal/httpapi/errors.go.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace sb::httpapi {

// uWS wants the full status line ("404 Not Found"), not a bare code — Go's
// net/http derives that automatically from the int; here we spell out the
// small fixed set this API ever returns.
constexpr const char *kStatus200 = "200 OK";
constexpr const char *kStatus400 = "400 Bad Request";
constexpr const char *kStatus401 = "401 Unauthorized";
constexpr const char *kStatus413 = "413 Payload Too Large";
constexpr const char *kStatus429 = "429 Too Many Requests";
constexpr const char *kStatus500 = "500 Internal Server Error";
constexpr const char *kStatus503 = "503 Service Unavailable";

// IMPORTANT: uWS commits the response status line the first time ANY header
// or body byte is written — a writeStatus() call after that point is
// silently ignored (the client still sees "200 OK" on the wire even though
// the JSON body says otherwise). Every response must therefore call
// writeStatus() before its first writeHeader()/end(): these two helpers are
// the ONLY place that may call writeStatus, and every handler must go
// through them (including for the X-Request-ID header — never write it
// eagerly before the final status is known).
template <typename Res>
void WriteJSON(Res *res, const char *statusLine, const std::string &requestId, const nlohmann::ordered_json &v) {
  auto *r = res->writeStatus(statusLine)->writeHeader("Content-Type", "application/json");
  if (!requestId.empty()) r = r->writeHeader("X-Request-ID", requestId);
  r->end(v.dump());
}

template <typename Res>
void WriteError(Res *res, const char *statusLine, const std::string &code, const std::string &msg,
                 const std::string &stage, const std::string &requestId) {
  nlohmann::ordered_json body;
  body["error"]["code"] = code;
  body["error"]["message"] = msg;
  if (!stage.empty()) body["error"]["stage"] = stage;
  if (!requestId.empty()) body["error"]["request_id"] = requestId;
  WriteJSON(res, statusLine, requestId, body);
}

// For queue_full's Retry-After header — the only error response that needs
// an extra header, so this stays a one-off rather than a general mechanism.
template <typename Res>
void WriteErrorRetryAfter(Res *res, const char *statusLine, const std::string &code, const std::string &msg,
                           const std::string &requestId, const char *retryAfterSeconds) {
  nlohmann::ordered_json body;
  body["error"]["code"] = code;
  body["error"]["message"] = msg;
  if (!requestId.empty()) body["error"]["request_id"] = requestId;
  auto *r = res->writeStatus(statusLine)
                ->writeHeader("Content-Type", "application/json")
                ->writeHeader("Retry-After", retryAfterSeconds);
  if (!requestId.empty()) r = r->writeHeader("X-Request-ID", requestId);
  r->end(body.dump());
}

}  // namespace sb::httpapi
