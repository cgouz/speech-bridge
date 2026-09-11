#include "session.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "../text/unicode_lite.h"

namespace sb::httpapi {

namespace {

std::vector<float> DecodeFrame(const uint8_t *data, size_t len, const std::string &format) {
  std::vector<float> out;
  if (format == "s16") {
    size_t n = len / 2;
    out.resize(n);
    for (size_t i = 0; i < n; i++) {
      int16_t v = static_cast<int16_t>(static_cast<uint16_t>(data[i * 2]) | (static_cast<uint16_t>(data[i * 2 + 1]) << 8));
      out[i] = static_cast<float>(v) / 32768.0f;
    }
  } else {  // f32 little-endian
    size_t n = len / 4;
    out.resize(n);
    for (size_t i = 0; i < n; i++) {
      uint32_t bits = static_cast<uint32_t>(data[i * 4]) | (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
                      (static_cast<uint32_t>(data[i * 4 + 2]) << 16) | (static_cast<uint32_t>(data[i * 4 + 3]) << 24);
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      out[i] = f;
    }
  }
  return out;
}

// Runs `fn` when the enclosing scope exits (any return path) — C++ has no
// `defer`; this stands in for Go's `defer s.pending.Done()`.
struct ScopeExit {
  std::function<void()> fn;
  ~ScopeExit() {
    if (fn) fn();
  }
};

}  // namespace

nlohmann::ordered_json ToJSON(const Msg &m) {
  nlohmann::ordered_json j;
  j["type"] = m.type;
  if (!m.text.empty()) j["text"] = m.text;
  j["seq"] = m.seq;
  if (m.t0_ms != 0) j["t0_ms"] = m.t0_ms;
  if (m.t1_ms != 0) j["t1_ms"] = m.t1_ms;
  if (m.n_samples != 0) j["n_samples"] = m.n_samples;
  if (m.sample_rate != 0) j["sample_rate"] = m.sample_rate;
  if (!m.code.empty()) j["code"] = m.code;
  if (!m.stage.empty()) j["stage"] = m.stage;
  return j;
}

Session::Session(pipeline::Pipeline *p, Start s, std::function<void(Msg)> send)
    : pipe_(p), send_(std::move(send)), cfg_(std::move(s)) {
  if (cfg_.sample_rate == 0) cfg_.sample_rate = 16000;
  if (cfg_.format.empty()) cfg_.format = "f32";
  if (pipe_->HasSTT()) {
    stt_ = pipe_->NewSTTStream(cfg_.source_lang);
  }
}

void Session::PushAudio(const uint8_t *data, size_t len) {
  if (!stt_) return;
  std::vector<float> pcm = DecodeFrame(data, len, cfg_.format);
  if (cfg_.sample_rate != 16000) {
    pcm = pipeline::ResampleLinear(pcm, cfg_.sample_rate, 16000);
  }
  std::vector<core::STTEvent> evs;
  try {
    evs = stt_->Feed(pcm);
  } catch (const std::exception &e) {
    Msg m;
    m.type = "error";
    m.code = "stt_error";
    m.stage = "stt";
    m.text = e.what();
    send_(m);
    return;
  }
  HandleEvents(evs);
}

void Session::Stop() {
  if (stop_called_.exchange(true)) return;  // idempotent: "stop" message + close() both call this
  if (stt_) {
    try {
      HandleEvents(stt_->Finish());
    } catch (const std::exception &) {
      // matches Go: Finish()'s error is swallowed here, only success events handled
    }
  }
  // sb_stt_finish() is documented to never fabricate an <EOU> — if the model
  // genuinely never closed out the last sentence (e.g. the speaker stopped
  // talking and immediately hit Stop, with no silence gap for the model's
  // own EOU to fire on), last_partial_text_ still holds it. Force it through
  // here rather than silently dropping the tail of what was said.
  if (!last_partial_text_.empty()) {
    DispatchGrowingText(last_partial_text_, last_partial_end_ms_, last_partial_end_ms_, /*isFinal=*/true);
    dispatched_len_ = 0;
    chunk_open_ms_ = -1;
    last_partial_text_.clear();
  }
  stopping_.store(true);
  if (pending_.load() == 0) {
    FlushReorder();
    Msg done;
    done.type = "done";
    done.seq = PeekSeq();
    send_(done);
  }
  // else: the last worker to finish calls WorkerDone(), which sees
  // stopping_ set and pending_ reach zero, and sends "done" itself.
}

void Session::HandleEvents(const std::vector<core::STTEvent> &evs) {
  for (auto &e : evs) {
    if (e.kind == core::STTEventKind::kPartial) {
      Msg m;
      m.type = "partial";
      m.text = e.text;
      m.seq = PeekSeq();
      m.t1_ms = e.end_ms;
      send_(m);

      last_partial_text_ = e.text;
      last_partial_end_ms_ = e.end_ms;

      DispatchGrowingText(e.text, e.start_ms, e.end_ms, /*isFinal=*/false);
    } else if (e.kind == core::STTEventKind::kFinalEOU) {
      // The utterance is genuinely over now — every sentence found, plus any
      // trailing unpunctuated fragment, is safe to dispatch.
      DispatchGrowingText(e.text, e.start_ms, e.end_ms, /*isFinal=*/true);
      dispatched_len_ = 0;
      chunk_open_ms_ = -1;
      last_partial_text_.clear();
    }
  }
}

void Session::DispatchGrowingText(const std::string &text, int64_t t0, int64_t t1, bool isFinal) {
  auto sentences = pipeline::Sentences(text);
  // Non-final: hold back the last sentence found until a *later* call proves
  // it wasn't actually the still-growing trailing fragment (see the class
  // doc comment on HandleEvents) — a streaming ASR model doesn't revise text
  // once new words follow it, so only corroborated sentences are trustworthy
  // here. Final (<EOU>/Stop): nothing is still growing, trust all of them.
  size_t confirmed = sentences.empty() ? 0 : sentences.size() - (isFinal ? 0 : 1);

  size_t searchFrom = 0;
  for (size_t i = 0; i < confirmed; i++) {
    size_t pos = text.find(sentences[i], searchFrom);
    size_t startOff = (pos == std::string::npos) ? searchFrom : pos;
    size_t endOff = (pos == std::string::npos) ? searchFrom : pos + sentences[i].size();
    if (startOff >= dispatched_len_) {
      Dispatch(sentences[i], t0, t1);
      dispatched_len_ = endOff;
      chunk_open_ms_ = -1;
    } else if (endOff > dispatched_len_) {
      // This sentence only came into existence because punctuation was
      // recognized just after a kMaxChunkLatencyMs timeout already forced
      // its opening words out as a raw chunk (see below) — re-dispatching
      // the whole sentence now would speak that overlap twice. Absorb the
      // (small, already-implied) trailing words into dispatched_len_ instead
      // of re-translating them: a duplicate sentence spoken twice is far
      // more jarring in a live call than a couple of trailing words silently
      // folded into the chunk that already covered them.
      dispatched_len_ = endOff;
      chunk_open_ms_ = -1;
    }
    searchFrom = endOff;
  }

  std::string tail = sb::text::TrimSpace(text.substr(std::min(dispatched_len_, text.size())));
  if (tail.empty()) {
    chunk_open_ms_ = -1;
    return;
  }

  if (isFinal) {
    Dispatch(tail, t0, t1);
    return;
  }

  // Time-based fallback: bound how long a run-on utterance can withhold
  // translation waiting for punctuation that streaming ASR may never surface
  // mid-utterance (see the class doc comment on HandleEvents).
  if (chunk_open_ms_ < 0) {
    chunk_open_ms_ = t1;
    return;
  }
  if (t1 - chunk_open_ms_ >= kMaxChunkLatencyMs) {
    Dispatch(tail, t0, t1);
    dispatched_len_ = text.size();
    chunk_open_ms_ = -1;
  }
}

void Session::Dispatch(const std::string &sentence, int64_t t0, int64_t t1) {
  int seq;
  {
    std::lock_guard<std::mutex> g(mu_);
    seq = seq_++;
  }

  Msg transcript;
  transcript.type = "transcript";
  transcript.text = sentence;
  transcript.seq = seq;
  transcript.t0_ms = t0;
  transcript.t1_ms = t1;
  send_(transcript);

  pending_.fetch_add(1);
  // `self` keeps the Session alive for this thread's duration even if the WS
  // connection (and therefore stream.cpp's shared_ptr to this Session) is
  // gone by the time MT/TTS finishes — see the class doc comment.
  std::thread([self = shared_from_this(), sentence, seq]() {
    ScopeExit onExit{[self] { self->WorkerDone(); }};

    std::string src = self->cfg_.source_lang.empty() ? "auto" : self->cfg_.source_lang;
    std::string translated = sentence;
    try {
      auto out = self->pipe_->Translate(sentence, src, self->cfg_.target_lang);
      if (out.ok) {
        translated = pipeline::ForDisplay(out.text, self->cfg_.target_lang);
        Msg m;
        m.type = "translation";
        m.text = translated;
        m.seq = seq;
        self->send_(m);
      } else {
        Msg m;
        m.type = "error";
        m.code = "mt_unavailable";
        m.stage = "mt";
        m.seq = seq;
        self->send_(m);
      }
    } catch (const std::exception &e) {
      Msg m;
      m.type = "error";
      m.code = "mt_error";
      m.stage = "mt";
      m.seq = seq;
      m.text = e.what();
      self->send_(m);
    }

    try {
      auto sr = self->pipe_->Synth(pipeline::ForTTS(translated, self->cfg_.target_lang), self->cfg_.target_lang,
                                    self->cfg_.voice);
      if (!sr.ok) {
        Msg m;
        m.type = "error";
        m.code = "tts_unavailable";
        m.stage = "tts";
        m.seq = seq;
        self->send_(m);
        self->MarkReorder(seq, {}, 0);
        return;
      }
      self->MarkReorder(seq, std::move(sr.pcm), sr.sample_rate);
    } catch (const std::exception &e) {
      Msg m;
      m.type = "error";
      m.code = "tts_error";
      m.stage = "tts";
      m.seq = seq;
      m.text = e.what();
      self->send_(m);
      self->MarkReorder(seq, {}, 0);
    }
  }).detach();
}

void Session::WorkerDone() {
  if (pending_.fetch_sub(1) == 1 && stopping_.load()) {
    // We were the last in-flight worker and Stop() is waiting on us.
    FlushReorder();
    Msg done;
    done.type = "done";
    done.seq = PeekSeq();
    send_(done);
  }
}

void Session::MarkReorder(int seq, std::vector<float> pcm, int sr) {
  std::lock_guard<std::mutex> g(mu_);
  reorder_[seq] = std::move(pcm);
  reorder_sr_[seq] = sr;
  for (;;) {
    auto it = reorder_.find(next_emit_);
    if (it == reorder_.end()) return;
    std::vector<float> p = std::move(it->second);
    int r = reorder_sr_[next_emit_];
    reorder_.erase(it);
    reorder_sr_.erase(next_emit_);
    if (!p.empty()) {
      Msg m;
      m.type = "audio";
      m.seq = next_emit_;
      m.n_samples = static_cast<int>(p.size());
      m.sample_rate = r;
      m.binary = std::move(p);
      send_(m);
    }
    next_emit_++;
  }
}

void Session::FlushReorder() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<int> keys;
  for (auto &[k, v] : reorder_) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (int k : keys) {
    auto &pcm = reorder_[k];
    if (!pcm.empty()) {
      Msg m;
      m.type = "audio";
      m.seq = k;
      m.n_samples = static_cast<int>(pcm.size());
      m.sample_rate = reorder_sr_[k];
      m.binary = std::move(pcm);
      send_(m);
    }
    reorder_.erase(k);
  }
}

int Session::PeekSeq() {
  std::lock_guard<std::mutex> g(mu_);
  return seq_;
}

}  // namespace sb::httpapi
