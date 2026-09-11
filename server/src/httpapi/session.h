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
  // revise text once new words have been recognized after it. The trailing
  // fragment itself only dispatches once the true <EOU> confirms the
  // utterance is over (see dispatched_in_utterance_).
  void HandleEvents(const std::vector<core::STTEvent> &evs);
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
  // Count of sentences already dispatched from the CURRENT open utterance
  // (see HandleEvents) — reset to 0 once the model's <EOU> closes it out.
  // Only ever touched from PushAudio/Stop, both on the event-loop thread, so
  // this needs no lock (unlike seq_, which Dispatch's worker threads also read).
  size_t dispatched_in_utterance_ = 0;
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
