// core — the four Speech Bridge engine interfaces, loaded via dlopen. Direct
// port of app/internal/core/core.go. Pure interfaces here (no dlopen); the
// concrete implementation is in native_core.h/.cpp, wrapping shim.h.
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace sb::core {

// Errors surfaced across the ABI. The C side returns negative sb_status
// codes; CoreError mirrors them (see statusErr in native_core.cpp).
class CoreError : public std::runtime_error {
 public:
  explicit CoreError(const std::string &msg) : std::runtime_error(msg) {}
};

enum class STTEventKind { kNone, kPartial, kFinalEOU };

struct STTEvent {
  STTEventKind kind = STTEventKind::kNone;
  std::string text;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
};

// STTStream is one speaker session. Not safe for concurrent use — the caller
// serializes Feed/Finish/Close (one inference at a time per context).
class STTStream {
 public:
  virtual ~STTStream() = default;
  virtual std::vector<STTEvent> Feed(const std::vector<float> &pcm) = 0;
  virtual std::vector<STTEvent> Finish() = 0;
};

// STT is a loaded streaming speech-to-text model. Weights load once; streams
// are cheap and hold per-session encoder/decoder cache.
class STT {
 public:
  virtual ~STT() = default;
  virtual std::unique_ptr<STTStream> NewStream(const std::string &lang) = 0;
};

// MTCtx translates ONE sentence per call. Not safe for concurrent use.
class MTCtx {
 public:
  virtual ~MTCtx() = default;
  virtual std::string Translate(const std::string &text, const std::string &src,
                                 const std::string &dst) = 0;
};

// MT is a loaded translation model (MADLAD-400). Contexts are cheap.
class MT {
 public:
  virtual ~MT() = default;
  virtual std::unique_ptr<MTCtx> NewCtx() = 0;
};

struct TTSResult {
  std::vector<float> pcm;
  int sample_rate = 0;
};

// TTSCtx synthesizes ONE sentence per call. Not safe for concurrent use.
class TTSCtx {
 public:
  virtual ~TTSCtx() = default;
  virtual TTSResult Speak(const std::string &text, const std::string &lang,
                           const std::string &voice) = 0;
};

// TTS is a loaded text-to-speech model — either magpie or vits, same interface.
class TTS {
 public:
  virtual ~TTS() = default;
  virtual std::unique_ptr<TTSCtx> NewCtx() = 0;
  // Languages this engine can synthesize.
  virtual std::vector<std::string> Languages() const = 0;
};

enum class TTSEngine { kMagpie, kVITS };

// Options configures which native cores to load and from where.
struct Options {
  std::string lib_dir;  // directory holding libsb_*.{so,dylib}
  std::string device;   // "cpu" | "metal" | "auto"
  std::string stt_model;    // empty = don't load STT
  std::string mt_model;
  int mt_ctx = 512;
  std::string magpie_model;
  std::string vits_dir;
};

// Set is the loaded collection of cores. Any field may be null (not
// configured or failed to load); report explains why.
struct Set {
  std::unique_ptr<STT> stt;
  std::unique_ptr<MT> mt;
  std::unique_ptr<TTS> tts_magpie;
  std::unique_ptr<TTS> tts_vits;
  std::map<std::string, std::string> report;  // core name -> "ok" or an error string

  bool Up(const std::string &name) const {
    auto it = report.find(name);
    return it != report.end() && it->second == "ok";
  }
};

// Load opens every configured core. Failures are recorded in Set::report and
// do not abort the others (degraded operation, per spec).
Set Load(const Options &o);

}  // namespace sb::core
