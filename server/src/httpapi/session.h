// session — per-connection state machine for a live speech-to-speech stream:
// one speaker, one target language. Transport-agnostic — stream.cpp adapts
// uWS WebSocket frames to these calls. Direct port of
// app/internal/session/session.go, adapted for uWS's single-threaded event
// loop: Session is reference-counted (std::shared_ptr) so a detached
// MT->TTS worker thread can keep it alive after the WS connection itself
// closes, and Stop()/close-cleanup never block the shared event loop thread
// waiting on workers — completion is signaled asynchronously instead (see
// WorkerDone).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../native/core.h"
#include "../pipeline/pipeline.h"

namespace sb::httpapi {

// Msg is a server->client event. binary carries the PCM frame that follows
// an "audio" header (empty for every other type).
struct Msg {
  std::string type;  // partial|transcript|translation|audio|error|done
  std::string text;
  int seq = 0;
  int64_t t0_ms = 0;
  int64_t t1_ms = 0;
  int n_samples = 0;
  int sample_rate = 0;
  std::string code;
  std::string stage;

  std::vector<float> binary;
};

nlohmann::ordered_json ToJSON(const Msg &m);

// Start is the client's opening frame.
struct Start {
  std::string type;
  std::string source_lang;
  std::string target_lang;
  std::string voice;
  int sample_rate = 0;
  std::string format;  // "f32" | "s16"
};

// Session drives one speaker stream. `send` must be safe to call from
// multiple threads — it is expected to marshal the actual write onto the WS
// connection's event-loop thread (see stream.cpp).
class Session : public std::enable_shared_from_this<Session> {
 public:
  // Throws core::CoreError if STT is configured but fails to open a stream.
  Session(pipeline::Pipeline *p, Start s, std::function<void(Msg)> send);

  void PushAudio(const uint8_t *data, size_t len);
  // Flushes the STT tail and requests a "done" once every in-flight sentence
  // worker (MT->TTS) finishes. Never blocks — safe to call from the event
  // loop thread.
  void Stop();

 private:
  // Dispatches every complete sentence in a growing utterance as soon as it's
  // safe to, rather than waiting for the STT model's own <EOU> — parakeet's
  // EOU model is trained for conversational turn-taking, not sentence
  // boundaries, and may not fire until the speaker falls silent for a while
  // (or the stream ends), which would otherwise hold every sentence in a
  // multi-sentence turn hostage until the whole thing is spoken. Safe rule:
  // any sentence text::SplitSentences finds *before* the still-growing
  // trailing fragment of a PARTIAL is final — a streaming ASR model doesn't
  // revise text once new words have been recognized after it.
  //
  // That alone isn't enough, though: SplitSentences only finds a boundary at
  // sentence-ending punctuation, and streaming ASR punctuation restoration is
  // unreliable mid-utterance (more so for non-English source languages) — a
  // long, continuous, run-on turn can go many seconds without ever producing
  // one, which would silently fall back to "wait for <EOU>/Stop" and defeat
  // real-time dispatch entirely. DispatchGrowingText also force-flushes the
  // open (unpunctuated) tail once it has sat undispatched for
  // kMaxChunkLatencyMs of audio time, the same rolling-chunk trade-off
  // real-time speech-to-speech products make (bounded latency over waiting
  // for a clean sentence boundary that may never come).
  void HandleEvents(const std::vector<core::STTEvent> &evs);
  // Dispatches whatever new, not-yet-dispatched text `text` (the STT event's
  // full growing-utterance transcript so far) contains — see the class doc
  // comment on HandleEvents. `isFinal` is true only for a genuine <EOU> or
  // Stop()'s force-flush: every sentence found (plus any trailing
  // unpunctuated fragment) is dispatched, no hold-back and no timer.
  void DispatchGrowingText(const std::string &text, int64_t t0, int64_t t1, bool isFinal);
  void Dispatch(const std::string &sentence, int64_t t0, int64_t t1);
  void MarkReorder(int seq, std::vector<float> pcm, int sr);
  void FlushReorder();
  int PeekSeq();
  // Called exactly once by each worker when it finishes (success or error).
  // Emits "done" once stopping_ is set and no worker remains in flight.
  void WorkerDone();

  pipeline::Pipeline *pipe_;
  std::function<void(Msg)> send_;
  std::unique_ptr<core::STTStream> stt_;
  Start cfg_;

  std::mutex mu_;  // guards seq_, reorder_, reorder_sr_, next_emit_
  int seq_ = 0;
  // Byte offset into the CURRENT open utterance's growing transcript already
  // handed to Dispatch() (see DispatchGrowingText) — reset to 0 once the
  // model's <EOU> closes the utterance out. A streaming ASR model only ever
  // extends its hypothesis with a stable prefix (see HandleEvents' doc
  // comment), so this offset stays valid as `text` grows across calls. Only
  // ever touched from PushAudio/Stop, both on the event-loop thread, so this
  // needs no lock (unlike seq_, which Dispatch's worker threads also read).
  size_t dispatched_len_ = 0;
  // Audio-timeline ms (STTEvent::end_ms) at which the current undispatched
  // open tail was first observed non-empty; -1 while there is no open tail.
  // DispatchGrowingText force-flushes the tail once "now" (the latest
  // event's end_ms) exceeds this by kMaxChunkLatencyMs, so a long run-on
  // utterance with no sentence-ending punctuation still gets translated
  // within a bounded delay instead of only at <EOU>/Stop.
  int64_t chunk_open_ms_ = -1;
  static constexpr int64_t kMaxChunkLatencyMs = 3000;
  // The most recent PARTIAL text and its end timestamp, kept so Stop() can
  // force-flush an undispatched trailing fragment if the stream ends before
  // the model ever fires a genuine <EOU> for it (sb_stt_finish() is
  // documented to never fabricate one — see Stop()'s comment).
  std::string last_partial_text_;
  int64_t last_partial_end_ms_ = 0;
  std::map<int, std::vector<float>> reorder_;
  std::map<int, int> reorder_sr_;
  int next_emit_ = 0;

  std::atomic<int> pending_{0};       // in-flight sentence workers
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stop_called_{false};  // Stop() runs its body at most once
};

}  // namespace sb::httpapi
