// native_core.cpp — dlopen-backed implementations of the core:: interfaces.
// Direct port of app/internal/core/native.go, calling into shim.h (itself a
// verbatim port of the Go build's shim.c/shim.h — pure C, no cgo-specific
// code, so it needed no translation at all).
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core.h"
#include "shim.h"

namespace sb::core {

namespace {

#if defined(__APPLE__)
constexpr const char *kLibExt = "dylib";
#else
constexpr const char *kLibExt = "so";
#endif

std::string LibPath(const std::string &dir, const std::string &base) {
  return dir + "/" + base + "." + kLibExt;
}

// Mirrors native.go's statusErr: the C side's negative sb_status codes.
std::string StatusMessage(int code) {
  switch (code) {
    case 0: return "ok";
    case -1: return "core: bad argument";
    case -2: return "core: model load failed";
    case -3: return "core: inference failed";
    case -4: return "core: unsupported language";
    case -5: return "core: context busy";
    default: return "core: inference failed";
  }
}

void OpenLib(int which, const std::string &path, const std::string &name) {
  char errbuf[512] = {0};
  int rc = 0;
  switch (which) {
    case 0: rc = sbn_open_stt(path.c_str(), errbuf, sizeof(errbuf)); break;
    case 1: rc = sbn_open_mt(path.c_str(), errbuf, sizeof(errbuf)); break;
    case 2: rc = sbn_open_tts_magpie(path.c_str(), errbuf, sizeof(errbuf)); break;
    case 3: rc = sbn_open_tts_vits(path.c_str(), errbuf, sizeof(errbuf)); break;
  }
  if (rc == 0) return;
  if (rc == 1) throw CoreError("core " + name + ": no library path");
  throw CoreError("core " + name + ": dlopen " + path + ": " + errbuf);
}

// ---- STT ----

class NativeSTTStream final : public STTStream {
 public:
  explicit NativeSTTStream(sb_stt_stream *s) : s_(s) {}
  ~NativeSTTStream() override {
    std::lock_guard<std::mutex> g(mu_);
    if (s_) {
      sbn_stt_stream_free(s_);
      s_ = nullptr;
    }
  }

  std::vector<STTEvent> Feed(const std::vector<float> &pcm) override {
    std::lock_guard<std::mutex> g(mu_);
    const float *p = pcm.empty() ? nullptr : pcm.data();
    sb_status rc = sbn_stt_feed(s_, p, pcm.size());
    if (rc != 0) {
      throw CoreError("sb_stt_feed: " + std::string(sbn_stt_last_error(s_)) +
                       " (" + StatusMessage(rc) + ")");
    }
    return Drain();
  }

  std::vector<STTEvent> Finish() override {
    std::lock_guard<std::mutex> g(mu_);
    sb_status rc = sbn_stt_finish(s_);
    if (rc != 0) {
      throw CoreError("sb_stt_finish: " + std::string(sbn_stt_last_error(s_)) +
                       " (" + StatusMessage(rc) + ")");
    }
    return Drain();
  }

 private:
  std::vector<STTEvent> Drain() {
    std::vector<STTEvent> evs;
    for (;;) {
      sb_stt_event ev{};
      sb_status rc = sbn_stt_poll(s_, &ev);
      if (rc != 0) return evs;  // matches Go: return what we have + drop error
      if (ev.kind == SB_STT_NONE) return evs;
      STTEvent e;
      e.kind = ev.kind == SB_STT_FINAL_EOU ? STTEventKind::kFinalEOU : STTEventKind::kPartial;
      e.text = ev.text ? ev.text : "";  // copied out immediately
      e.start_ms = ev.start_ms;
      e.end_ms = ev.end_ms;
      evs.push_back(std::move(e));
    }
  }

  std::mutex mu_;
  sb_stt_stream *s_;
};

class NativeSTT final : public STT {
 public:
  explicit NativeSTT(sb_stt_model *m) : m_(m) {}
  ~NativeSTT() override {
    if (m_) sbn_stt_model_free(m_);
  }

  std::unique_ptr<STTStream> NewStream(const std::string &lang) override {
    sb_stt_stream *s = sbn_stt_stream_new(m_, lang.c_str());
    if (!s) {
      throw CoreError("sb_stt_stream_new: " + std::string(sbn_stt_model_last_error(m_)));
    }
    return std::make_unique<NativeSTTStream>(s);
  }

 private:
  sb_stt_model *m_;
};

std::unique_ptr<STT> LoadNativeSTT(const std::string &libPath, const std::string &modelPath,
                                    const std::string &device) {
  OpenLib(0, libPath, "stt");
  sb_stt_model *m = sbn_stt_model_load(modelPath.c_str(), device.c_str());
  if (!m) throw CoreError("core: model load failed: sb_stt_model_load(" + modelPath + ")");
  return std::make_unique<NativeSTT>(m);
}

// ---- MT ----

class NativeMTCtx final : public MTCtx {
 public:
  explicit NativeMTCtx(sb_mt_ctx *c) : c_(c) {}
  ~NativeMTCtx() override {
    std::lock_guard<std::mutex> g(mu_);
    if (c_) {
      sbn_mt_ctx_free(c_);
      c_ = nullptr;
    }
  }

  std::string Translate(const std::string &text, const std::string &src,
                         const std::string &dst) override {
    std::lock_guard<std::mutex> g(mu_);
    constexpr size_t kCap = 16384;
    std::vector<char> buf(kCap);
    sb_status rc = sbn_mt_translate(c_, text.c_str(), src.c_str(), dst.c_str(), buf.data(), kCap);
    if (rc != 0) {
      throw CoreError("sb_mt_translate: " + std::string(sbn_mt_last_error(c_)) +
                       " (" + StatusMessage(rc) + ")");
    }
    return std::string(buf.data());
  }

 private:
  std::mutex mu_;
  sb_mt_ctx *c_;
};

class NativeMT final : public MT {
 public:
  explicit NativeMT(sb_mt_model *m) : m_(m) {}
  ~NativeMT() override {
    if (m_) sbn_mt_model_free(m_);
  }

  std::unique_ptr<MTCtx> NewCtx() override {
    sb_mt_ctx *c = sbn_mt_ctx_new(m_);
    if (!c) throw CoreError("sb_mt_ctx_new: " + std::string(sbn_mt_model_last_error(m_)));
    return std::make_unique<NativeMTCtx>(c);
  }

 private:
  sb_mt_model *m_;
};

std::unique_ptr<MT> LoadNativeMT(const std::string &libPath, const std::string &modelPath, int nCtx) {
  OpenLib(1, libPath, "mt");
  sb_mt_model *m = sbn_mt_model_load(modelPath.c_str(), nCtx);
  if (!m) throw CoreError("core: model load failed: sb_mt_model_load(" + modelPath + ")");
  return std::make_unique<NativeMT>(m);
}

// ---- TTS (magpie | vits, selected by an int engine index: 0 | 1) --------

class NativeTTSCtx final : public TTSCtx {
 public:
  NativeTTSCtx(int engine, sb_tts_ctx *c) : engine_(engine), c_(c) {}
  ~NativeTTSCtx() override {
    std::lock_guard<std::mutex> g(mu_);
    if (c_) {
      sbn_tts_ctx_free(engine_, c_);
      c_ = nullptr;
    }
  }

  TTSResult Speak(const std::string &text, const std::string &lang, const std::string &voice) override {
    std::lock_guard<std::mutex> g(mu_);
    const float *pcm = nullptr;
    size_t n = 0;
    int sr = 0;
    sb_status rc = sbn_tts_speak(engine_, c_, text.c_str(), lang.c_str(), voice.c_str(), &pcm, &n, &sr);
    if (rc != 0) {
      throw CoreError("sb_tts_speak: " + std::string(sbn_tts_last_error(engine_, c_)) +
                       " (" + StatusMessage(rc) + ")");
    }
    TTSResult out;
    out.sample_rate = sr;
    if (n > 0) out.pcm.assign(pcm, pcm + n);  // copy out before the next call invalidates it
    return out;
  }

 private:
  std::mutex mu_;
  int engine_;
  sb_tts_ctx *c_;
};

class NativeTTS final : public TTS {
 public:
  NativeTTS(int engine, sb_tts_model *m) : engine_(engine), m_(m) {
    constexpr size_t kCap = 512;
    std::vector<char> buf(kCap);
    if (sbn_tts_languages(engine_, m_, buf.data(), kCap) == 0) {
      std::string csv(buf.data());
      size_t start = 0;
      while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        std::string part = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // trim whitespace
        size_t a = part.find_first_not_of(" \t");
        size_t b = part.find_last_not_of(" \t");
        if (a != std::string::npos) langs_.push_back(part.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
  }
  ~NativeTTS() override {
    if (m_) sbn_tts_model_free(engine_, m_);
  }

  std::unique_ptr<TTSCtx> NewCtx() override {
    sb_tts_ctx *c = sbn_tts_ctx_new(engine_, m_);
    if (!c) throw CoreError("sb_tts_ctx_new: " + std::string(sbn_tts_model_last_error(engine_, m_)));
    return std::make_unique<NativeTTSCtx>(engine_, c);
  }

  std::vector<std::string> Languages() const override { return langs_; }

 private:
  int engine_;
  sb_tts_model *m_;
  std::vector<std::string> langs_;
};

std::unique_ptr<TTS> LoadNativeTTS(TTSEngine eng, const std::string &libPath, const std::string &modelPath,
                                    const std::string &device) {
  int engine = eng == TTSEngine::kMagpie ? 0 : 1;
  int which = eng == TTSEngine::kMagpie ? 2 : 3;
  std::string name = eng == TTSEngine::kMagpie ? "magpie" : "vits";
  OpenLib(which, libPath, name);
  sb_tts_model *m = sbn_tts_model_load(engine, modelPath.c_str(), device.c_str());
  if (!m) throw CoreError("core: model load failed: sb_tts_model_load(" + name + ", " + modelPath + ")");
  return std::make_unique<NativeTTS>(engine, m);
}

}  // namespace

Set Load(const Options &opt) {
  Options o = opt;
  if (o.mt_ctx <= 0) o.mt_ctx = 512;
  if (o.device.empty()) o.device = "auto";

  Set s;
  s.report = {{"stt", "not configured"},
              {"mt", "not configured"},
              {"tts_magpie", "not configured"},
              {"tts_vits", "not configured"}};

  if (!o.stt_model.empty()) {
    try {
      s.stt = LoadNativeSTT(LibPath(o.lib_dir, "libsb_stt"), o.stt_model, o.device);
      s.report["stt"] = "ok";
    } catch (const std::exception &e) {
      s.report["stt"] = e.what();
    }
  }
  if (!o.mt_model.empty()) {
    try {
      s.mt = LoadNativeMT(LibPath(o.lib_dir, "libsb_mt"), o.mt_model, o.mt_ctx);
      s.report["mt"] = "ok";
    } catch (const std::exception &e) {
      s.report["mt"] = e.what();
    }
  }
  if (!o.magpie_model.empty()) {
    try {
      s.tts_magpie = LoadNativeTTS(TTSEngine::kMagpie, LibPath(o.lib_dir, "libsb_tts_magpie"), o.magpie_model, o.device);
      s.report["tts_magpie"] = "ok";
    } catch (const std::exception &e) {
      s.report["tts_magpie"] = e.what();
    }
  }
  if (!o.vits_dir.empty()) {
    try {
      s.tts_vits = LoadNativeTTS(TTSEngine::kVITS, LibPath(o.lib_dir, "libsb_tts_vits"), o.vits_dir, o.device);
      s.report["tts_vits"] = "ok";
    } catch (const std::exception &e) {
      s.report["tts_vits"] = e.what();
    }
  }
  return s;
}

}  // namespace sb::core
