#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "../src/config.h"

namespace fs = std::filesystem;

namespace {

void ClearEnv() {
  for (const char *k : {"SB_BIND", "SB_MODELS_DIR", "SB_LIB_DIR", "SB_STT_MODEL", "SB_MT_MODEL",
                         "SB_TTS_MAGPIE_MODEL", "SB_TTS_VITS_DIR", "SB_MT_CTX", "SB_MAX_BODY_MB",
                         "SB_MAX_AUDIO_SEC", "SB_QUEUE_DEPTH", "SB_STREAMS_MAX", "SB_AUTH_TOKEN",
                         "SB_LOG_LEVEL", "SB_LOG_FORMAT", "SB_DEVICE"}) {
    unsetenv(k);
  }
}

fs::path TempDir() {
  static std::mt19937_64 rng{std::random_device{}()};
  fs::path p = fs::temp_directory_path() / ("sb_cpp_test_" + std::to_string(rng()));
  fs::create_directories(p);
  return p;
}

void MustFile(const fs::path &p) {
  std::ofstream(p) << "x";
}

}  // namespace

TEST_CASE("Config defaults", "[config]") {
  ClearEnv();
  fs::path dir = TempDir();
  setenv("SB_LIB_DIR", dir.c_str(), 1);
  setenv("SB_MODELS_DIR", dir.c_str(), 1);

  sb::Config c = sb::LoadConfig();
  CHECK(c.bind == "127.0.0.1:8080");
  CHECK(c.mt_ctx == 512);
  CHECK(c.max_body_mb == 25);
  CHECK(c.max_audio_sec == 120);
  CHECK(c.queue_depth == 8);
  CHECK(c.streams_max == 4);
  CHECK(c.log_format == "json");
  CHECK(c.log_level == "info");

  ClearEnv();
  fs::remove_all(dir);
}

TEST_CASE("Config rejects invalid values", "[config]") {
  ClearEnv();
  fs::path dir = TempDir();
  setenv("SB_LIB_DIR", dir.c_str(), 1);
  setenv("SB_BIND", "not-a-host-port", 1);
  setenv("SB_MT_CTX", "9", 1);
  setenv("SB_QUEUE_DEPTH", "0", 1);
  setenv("SB_LOG_LEVEL", "loud", 1);

  bool threw = false;
  try {
    sb::LoadConfig();
  } catch (const sb::ConfigError &e) {
    threw = true;
    std::string msg = e.what();
    for (const char *want : {"SB_BIND", "SB_MT_CTX", "SB_QUEUE_DEPTH", "SB_LOG_LEVEL"}) {
      INFO("expected error to mention " << want << "; got: " << msg);
      CHECK(msg.find(want) != std::string::npos);
    }
  }
  CHECK(threw);

  ClearEnv();
  fs::remove_all(dir);
}

TEST_CASE("Config auto-discovers models", "[config]") {
  ClearEnv();
  fs::path root = TempDir();
  setenv("SB_LIB_DIR", root.c_str(), 1);
  setenv("SB_MODELS_DIR", root.c_str(), 1);

  fs::create_directories(root / "stt");
  MustFile(root / "stt" / "nemotron.gguf");
  fs::create_directories(root / "tts_vits" / "vits-mms-rus");
  MustFile(root / "tts_vits" / "vits-mms-rus" / "model.onnx");

  sb::Config c = sb::LoadConfig();
  CHECK(fs::path(c.stt_model).filename() == "nemotron.gguf");
  CHECK(c.mt_model.empty());
  CHECK_FALSE(c.vits_dir.empty());

  ClearEnv();
  fs::remove_all(root);
}
