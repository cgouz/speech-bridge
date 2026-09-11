#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace sb {

namespace {

std::string Getenv(const char *key, const std::string &def) {
  const char *v = std::getenv(key);
  return v ? std::string(v) : def;
}

int Getint(const char *key, int def, std::vector<std::string> &errs) {
  const char *v = std::getenv(key);
  if (!v || !*v) return def;
  try {
    size_t pos = 0;
    int n = std::stoi(v, &pos);
    if (pos != std::strlen(v)) throw std::invalid_argument("trailing chars");
    return n;
  } catch (...) {
    std::ostringstream os;
    os << key << ": \"" << v << "\" is not an integer";
    errs.push_back(os.str());
    return def;
  }
}

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool FileExists(const std::string &p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

bool IsDir(const std::string &p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

// resolveFile returns explicit if set (validated to exist), else the
// lexicographically-first "*.gguf" file under modelsDir/sub, else "".
std::string ResolveFile(const std::string &explicitPath, const std::string &modelsDir, const std::string &sub) {
  if (!explicitPath.empty()) {
    return FileExists(explicitPath) ? explicitPath : "";  // explicit but missing -> disabled
  }
  fs::path dir = fs::path(modelsDir) / sub;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "";
  std::vector<std::string> matches;
  for (auto &entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (entry.is_regular_file() && entry.path().extension() == ".gguf") {
      matches.push_back(entry.path().string());
    }
  }
  std::sort(matches.begin(), matches.end());
  for (auto &m : matches) {
    if (FileExists(m)) return m;
  }
  return "";
}

bool HasVITSVoice(const std::string &dir) {
  std::error_code ec;
  for (auto &entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (entry.is_directory() && FileExists((entry.path() / "model.onnx").string())) return true;
  }
  return false;
}

}  // namespace

Config LoadConfig() {
  std::vector<std::string> errs;
  Config c;
  c.bind = Getenv("SB_BIND", "127.0.0.1:8080");
  c.models_dir = Getenv("SB_MODELS_DIR", "models");
  c.lib_dir = Getenv("SB_LIB_DIR", "lib");

  c.mt_ctx = Getint("SB_MT_CTX", 512, errs);
  c.max_body_mb = Getint("SB_MAX_BODY_MB", 25, errs);
  c.max_audio_sec = Getint("SB_MAX_AUDIO_SEC", 120, errs);
  c.queue_depth = Getint("SB_QUEUE_DEPTH", 8, errs);
  c.streams_max = Getint("SB_STREAMS_MAX", 4, errs);

  c.auth_token = Getenv("SB_AUTH_TOKEN", "");
  c.log_level = ToLowerAscii(Getenv("SB_LOG_LEVEL", "info"));
  c.log_format = ToLowerAscii(Getenv("SB_LOG_FORMAT", "json"));
  c.device = ToLowerAscii(Getenv("SB_DEVICE", "auto"));

  c.stt_model = ResolveFile(Getenv("SB_STT_MODEL", ""), c.models_dir, "stt");
  c.mt_model = ResolveFile(Getenv("SB_MT_MODEL", ""), c.models_dir, "mt");
  c.magpie_model = ResolveFile(Getenv("SB_TTS_MAGPIE_MODEL", ""), c.models_dir, "tts_magpie");
  std::string vitsDir = Getenv("SB_TTS_VITS_DIR", "");
  if (!vitsDir.empty()) {
    c.vits_dir = vitsDir;
  } else {
    std::string dd = (fs::path(c.models_dir) / "tts_vits").string();
    if (IsDir(dd) && HasVITSVoice(dd)) c.vits_dir = dd;
  }

  // Validate SB_BIND as host:port (net.SplitHostPort equivalent: exactly one
  // ':' with a non-empty host and a numeric port).
  {
    size_t colon = c.bind.rfind(':');
    bool ok = colon != std::string::npos && colon > 0 && colon + 1 < c.bind.size();
    if (ok) {
      for (char ch : c.bind.substr(colon + 1)) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
          ok = false;
          break;
        }
      }
    }
    if (!ok) errs.push_back("SB_BIND: address " + c.bind + ": missing or invalid port");
  }
  if (c.mt_ctx < 64 || c.mt_ctx > 4096) {
    errs.push_back("SB_MT_CTX: " + std::to_string(c.mt_ctx) + " out of range [64,4096]");
  }
  for (auto &p : std::vector<std::pair<std::string, int>>{
           {"SB_MAX_BODY_MB", c.max_body_mb},
           {"SB_MAX_AUDIO_SEC", c.max_audio_sec},
           {"SB_QUEUE_DEPTH", c.queue_depth},
           {"SB_STREAMS_MAX", c.streams_max}}) {
    if (p.second <= 0) errs.push_back(p.first + ": must be > 0, got " + std::to_string(p.second));
  }
  if (c.log_level != "debug" && c.log_level != "info" && c.log_level != "warn" && c.log_level != "error") {
    errs.push_back("SB_LOG_LEVEL: \"" + c.log_level + "\" (want debug|info|warn|error)");
  }
  if (c.log_format != "json" && c.log_format != "text") {
    errs.push_back("SB_LOG_FORMAT: \"" + c.log_format + "\" (want json|text)");
  }
  if (c.device != "cpu" && c.device != "metal" && c.device != "cuda" && c.device != "auto") {
    errs.push_back("SB_DEVICE: \"" + c.device + "\" (want cpu|metal|cuda|auto)");
  }
  if (!IsDir(c.lib_dir)) {
    errs.push_back("SB_LIB_DIR: \"" + c.lib_dir + "\" is not a directory");
  }

  if (!errs.empty()) {
    std::sort(errs.begin(), errs.end());
    std::ostringstream os;
    os << "invalid configuration:\n";
    for (auto &e : errs) os << "  - " << e << "\n";
    std::string msg = os.str();
    if (!msg.empty() && msg.back() == '\n') msg.pop_back();
    throw ConfigError(msg);
  }
  return c;
}

}  // namespace sb
