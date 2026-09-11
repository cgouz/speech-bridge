#include "pipeline.h"

#include <algorithm>
#include <chrono>
#include <cctype>

#include "../text/split.h"
#include "../text/translit.h"
#include "../text/unicode_lite.h"

namespace sb::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

int64_t MsSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string ToLowerTrim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  std::string t = s.substr(a, b - a + 1);
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
  return t;
}

// normLang mirrors pipeline.go's normLang.
std::string NormLang(const std::string &l) {
  std::string t = ToLowerTrim(l);
  if (t == "pt" || t == "pt_br") return "pt-BR";
  return t;
}

// mtLang maps a display language code to the code MADLAD's "<2xx>" prompt
// expects (e.g. pt-BR -> pt).
std::string MtLang(const std::string &dst) {
  if (NormLang(dst) == "pt-BR") return "pt";
  return ToLowerTrim(dst);
}

bool HasCyrillic(const std::string &utf8) {
  for (char32_t r : text::Utf8Decode(utf8)) {
    if (r >= 0x0400 && r <= 0x04FF) return true;
  }
  return false;
}

// worstStage keeps the "most degraded" of two stage labels.
std::string WorstStage(const std::string &a, const std::string &b) {
  auto rank = [](const std::string &s) -> int {
    if (s == Pipeline::kStageOK) return 0;
    if (s == Pipeline::kStageMTUnavailable || s == Pipeline::kStageTTSUnavailable) return 1;
    if (s == Pipeline::kStageEmpty) return 2;
    return 0;
  };
  return rank(b) > rank(a) ? b : a;
}

}  // namespace

Pipeline::Pipeline(Engines eng) : eng_(eng) {
  if (eng_.mt) mt_ctx_ = eng_.mt->NewCtx();
  if (eng_.magpie) {
    magpie_ctx_ = eng_.magpie->NewCtx();
    for (auto &l : eng_.magpie->Languages()) route_[NormLang(l)] = core::TTSEngine::kMagpie;
  }
  if (eng_.vits) {
    vits_ctx_ = eng_.vits->NewCtx();
    for (auto &l : eng_.vits->Languages()) route_[NormLang(l)] = core::TTSEngine::kVITS;
  }
}

std::map<std::string, std::vector<std::string>> Pipeline::Capabilities() const {
  std::map<std::string, std::vector<std::string>> out;
  if (eng_.magpie) out["magpie"] = eng_.magpie->Languages();
  if (eng_.vits) out["vits"] = eng_.vits->Languages();
  return out;
}

std::unique_ptr<core::STTStream> Pipeline::NewSTTStream(std::string lang) {
  if (!eng_.stt) throw NoSTTError();
  if (lang.empty()) lang = "auto";
  return eng_.stt->NewStream(lang);
}

bool Pipeline::TTSForLang(const std::string &dst) const { return route_.count(NormLang(dst)) > 0; }

Pipeline::TranslateResult Pipeline::Translate(const std::string &sentence, const std::string &src,
                                               const std::string &dst) {
  if (!mt_ctx_) return {sentence, false};
  std::lock_guard<std::mutex> g(mt_mu_);
  return {mt_ctx_->Translate(sentence, src, MtLang(dst)), true};
}

Pipeline::SynthResult Pipeline::Synth(const std::string &sentence, const std::string &dst,
                                      const std::string &voice) {
  auto it = route_.find(NormLang(dst));
  if (it == route_.end()) return {};
  SynthResult r;
  r.ok = true;
  if (it->second == core::TTSEngine::kMagpie) {
    std::lock_guard<std::mutex> g(magpie_mu_);
    auto res = magpie_ctx_->Speak(sentence, dst, voice);
    r.pcm = std::move(res.pcm);
    r.sample_rate = res.sample_rate;
  } else {
    std::lock_guard<std::mutex> g(vits_mu_);
    auto res = vits_ctx_->Speak(sentence, dst, voice);
    r.pcm = std::move(res.pcm);
    r.sample_rate = res.sample_rate;
  }
  return r;
}

std::string ForDisplay(const std::string &s, const std::string &dst) {
  if (NormLang(dst) == "uz" && HasCyrillic(s)) return text::CyrillicToLatin(s);
  return s;
}

std::string ForTTS(const std::string &s, const std::string & /*dst*/) { return s; }

std::vector<std::string> Sentences(const std::string &transcript) { return text::SplitSentences(transcript); }

// ---- batch.go ----

std::pair<std::string, int64_t> Pipeline::Transcribe(const std::vector<float> &audio, int sampleRate,
                                                       const std::string &srcLangIn) {
  if (!eng_.stt) throw NoSTTError();
  std::string srcLang = srcLangIn.empty() ? "auto" : srcLangIn;
  auto start = Clock::now();
  std::string t = Transcribe_(audio, sampleRate, srcLang);
  return {t, MsSince(start)};
}

std::string Pipeline::Transcribe_(const std::vector<float> &audio, int sampleRate, const std::string &srcLang) {
  std::vector<float> pcm = sampleRate != kSTTSampleRate ? ResampleLinear(audio, sampleRate, kSTTSampleRate) : audio;

  auto stream = eng_.stt->NewStream(srcLang);
  std::vector<std::string> parts;
  auto collect = [&](const std::vector<core::STTEvent> &evs) {
    for (auto &e : evs) {
      if (e.kind == core::STTEventKind::kFinalEOU) {
        std::string t = text::TrimSpace(e.text);
        if (!t.empty()) parts.push_back(t);
      }
    }
  };

  for (size_t off = 0; off < pcm.size(); off += kChunkSamples) {
    size_t end = std::min(off + static_cast<size_t>(kChunkSamples), pcm.size());
    std::vector<float> chunk(pcm.begin() + off, pcm.begin() + end);
    collect(stream->Feed(chunk));
  }
  collect(stream->Finish());

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += " ";
    out += parts[i];
  }
  return out;
}

Pipeline::BatchResult Pipeline::Batch(const BatchInput &in) {
  if (!eng_.stt) throw NoSTTError();
  auto start = Clock::now();
  BatchResult res;
  res.stage = kStageOK;

  std::string src = in.source_lang.empty() ? "auto" : in.source_lang;

  auto sttStart = Clock::now();
  res.transcript = Transcribe_(in.audio, in.sample_rate, src);
  res.timings.stt_ms = MsSince(sttStart);
  if (text::TrimSpace(res.transcript).empty()) {
    res.stage = kStageEmpty;
    res.timings.total_ms = MsSince(start);
    return res;
  }

  auto sentences = Sentences(res.transcript);
  std::vector<std::string> translated;
  std::vector<float> audio;
  int sampleRate = 0;

  for (auto &s : sentences) {
    auto mtStart = Clock::now();
    std::string out = s;
    bool ok = false;
    try {
      auto tr = Translate(s, src, in.target_lang);
      out = tr.text;
      ok = tr.ok;
    } catch (const std::exception &) {
      out = s;
      ok = false;  // a per-sentence MT failure is degraded, not fatal
    }
    res.timings.mt_ms += MsSince(mtStart);
    std::string display = out;
    if (ok) {
      display = ForDisplay(out, in.target_lang);
    } else {
      res.stage = WorstStage(res.stage, kStageMTUnavailable);
    }
    translated.push_back(display);

    auto ttsStart = Clock::now();
    bool spoke = false;
    SynthResult sr;
    try {
      sr = Synth(ForTTS(out, in.target_lang), in.target_lang, in.voice);
      spoke = sr.ok;
    } catch (const std::exception &) {
      spoke = false;
    }
    res.timings.tts_ms += MsSince(ttsStart);
    if (!spoke) {
      res.stage = WorstStage(res.stage, kStageTTSUnavailable);
      continue;
    }
    if (sampleRate == 0) sampleRate = sr.sample_rate;
    audio.insert(audio.end(), sr.pcm.begin(), sr.pcm.end());
  }

  for (size_t i = 0; i < translated.size(); i++) {
    if (i) res.translation += " ";
    res.translation += translated[i];
  }
  res.audio = std::move(audio);
  res.sample_rate = sampleRate;
  res.timings.total_ms = MsSince(start);
  return res;
}

}  // namespace sb::pipeline
