// obs::Logger — structured logging with request_id / session_id tagging and
// a tiny dependency-free registry (metrics.h). Adapted from
// app/internal/obs/log.go: Go's implicit context.Context propagation becomes
// an explicit Logger value here (idiomatic C++ has no ambient context), built
// once per request/session via With() and threaded through explicitly.
//
// Transcript and translation text must only be logged at debug level — user
// speech is private data (mission rule) — see the Fields helpers below.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sb::obs {

enum class Level { kDebug, kInfo, kWarn, kError };

using Field = std::pair<std::string, nlohmann::ordered_json>;
using Fields = std::vector<Field>;

class Logger {
 public:
  // format: "json" | "text"; level: "debug"|"info"|"warn"|"error".
  Logger(std::string format, std::string level);

  // Returns a copy with `fields` merged in, attached to every subsequent
  // line — mirrors obs.From(ctx, base).With("request_id", v).
  Logger With(const Fields &fields) const;

  void Debug(const std::string &msg, const Fields &fields = {}) const;
  void Info(const std::string &msg, const Fields &fields = {}) const;
  void Warn(const std::string &msg, const Fields &fields = {}) const;
  void Error(const std::string &msg, const Fields &fields = {}) const;

  bool DebugEnabled() const { return level_ <= Level::kDebug; }

 private:
  void Log(Level lvl, const std::string &msg, const Fields &fields) const;

  std::string format_;
  Level level_;
  Fields base_fields_;
};

// Generates a random 12-byte hex id, used for request_id / session_id —
// mirrors httpapi's newID().
std::string NewID();

}  // namespace sb::obs
