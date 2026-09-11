// config — loads and validates the SB_* environment. Direct port of
// app/internal/config/config.go. Invalid values make the process exit at
// startup, never at request time.
#pragma once

#include <stdexcept>
#include <string>

namespace sb {

struct Config {
  std::string bind;

  std::string models_dir;
  std::string lib_dir;

  std::string stt_model;  // resolved file path ("" = STT disabled)
  std::string mt_model;
  std::string magpie_model;
  std::string vits_dir;

  int mt_ctx = 0;
  int max_body_mb = 0;
  int max_audio_sec = 0;
  int queue_depth = 0;
  int streams_max = 0;

  std::string auth_token;

  std::string log_level;   // debug|info|warn|error
  std::string log_format;  // json|text

  std::string device;  // cpu|metal|auto
};

class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(const std::string &msg) : std::runtime_error(msg) {}
};

// Load reads the environment and returns a validated Config, or throws
// ConfigError listing every problem found.
Config LoadConfig();

}  // namespace sb
