// pipeline — runs the per-sentence STT -> MT -> TTS chain in-process. Direct
// port of app/internal/pipeline/{pipeline,batch,audio}.go.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../native/core.h"

namespace sb::pipeline {

// Engines is the set of loaded cores the pipeline drives (non-owning — the
// caller, main.cpp, keeps the core::Set alive for the process lifetime).
struct Engines {
  core::STT *stt = nullptr;
  core::MT *mt = nullptr;
  core::TTS *magpie = nullptr;
  core::TTS *vits = nullptr;
};

// ---- audio.go ----
std::vector<float> ResampleLinear(const std::vector<float> &in, int from, int to);
std::vector<float> DownmixToMono(const std::vector<float> &interleaved, int channels);
constexpr int kSTTSampleRate = 16000;
constexpr int kChunkSamples = kSTTSampleRate / 4;  // ~250ms

// Pipeline holds the long-lived, gated inference contexts.
class Pipeline {
 public:
  // Throws core::CoreError if a configured engine's context fails to create.
  explicit Pipeline(Engines eng);

  std::map<std::string, std::vector<std::string>> Capabilities() const;
  bool HasSTT() const { return eng_.stt != nullptr; }
  bool HasMT() const { return mt_ctx_ != nullptr; }

  std::unique_ptr<core::STTStream> NewSTTStream(std::string lang);

  // Whether some loaded engine covers dst.
  bool TTSForLang(const std::string &dst) const;

  // Translate runs one sentence through MT (gated). If MT is not loaded,
  // returns {sentence, false}. Throws core::CoreError on a real MT failure.
  struct TranslateResult {
    std::string text;
    bool ok;
  };
  TranslateResult Translate(const std::string &sentence, const std::string &src, const std::string &dst);

  // Synth runs one sentence through the routed TTS engine (gated). Returns
  // ok=false (no exception) when no loaded engine covers dst. Throws
  // core::CoreError on a real TTS failure.
  struct SynthResult {
    std::vector<float> pcm;
    int sample_rate = 0;
    bool ok = false;
  };
  SynthResult Synth(const std::string &sentence, const std::string &dst, const std::string &voice);

  // ---- batch.go ----
  static constexpr const char *kStageOK = "ok";
  static constexpr const char *kStageEmpty = "empty_transcript";
  static constexpr const char *kStageMTUnavailable = "mt_unavailable";
  static constexpr const char *kStageTTSUnavailable = "tts_unavailable";

  struct BatchInput {
    std::vector<float> audio;  // mono float32
    int sample_rate = 0;
    std::string source_lang;  // "" or "auto" allowed
    std::string target_lang;
    std::string voice;
  };

  struct Timings {
    int64_t stt_ms = 0, mt_ms = 0, tts_ms = 0, total_ms = 0;
  };

  struct BatchResult {
    std::string transcript;
    std::string translation;
    std::vector<float> audio;
    int sample_rate = 0;
    Timings timings;
    std::string stage = kStageOK;
  };

  // Throws NoSTTError if no STT engine is loaded.
  BatchResult Batch(const BatchInput &in);
  // Throws NoSTTError if no STT engine is loaded.
  std::pair<std::string, int64_t> Transcribe(const std::vector<float> &audio, int sampleRate,
                                              const std::string &srcLang);

 private:
  std::string Transcribe_(const std::vector<float> &audio, int sampleRate, const std::string &srcLang);

  Engines eng_;
  std::mutex mt_mu_;
  std::unique_ptr<core::MTCtx> mt_ctx_;
  std::mutex magpie_mu_;
  std::unique_ptr<core::TTSCtx> magpie_ctx_;
  std::mutex vits_mu_;
  std::unique_ptr<core::TTSCtx> vits_ctx_;
  std::map<std::string, core::TTSEngine> route_;  // language -> engine
};

// Thrown by Batch/Transcribe when no STT engine is loaded (maps to HTTP 503).
class NoSTTError : public std::runtime_error {
 public:
  NoSTTError() : std::runtime_error("pipeline: STT engine not available") {}
};

// ---- free functions mirroring pipeline.go's package-level funcs ----

// ForDisplay converts a translated sentence to the script used for captions:
// MADLAD emits Uzbek in Cyrillic, but uz is shown in Latin.
std::string ForDisplay(const std::string &s, const std::string &dst);
// ForTTS returns the sentence in the script the target voice expects.
std::string ForTTS(const std::string &s, const std::string &dst);
// Sentences splits a transcript into pipeline units.
std::vector<std::string> Sentences(const std::string &transcript);

}  // namespace sb::pipeline
