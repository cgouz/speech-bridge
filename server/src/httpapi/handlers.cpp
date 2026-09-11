#include "handlers.h"

#include <nlohmann/json.hpp>
#include <sstream>

#include "errors.h"
#include "multipart.h"
#include "wav.h"
#include "../obs/log.h"

using json = nlohmann::ordered_json;

namespace sb::httpapi {

namespace {

std::string RouteLabel(const std::string &path) {
  if (path == "/health" || path == "/ready" || path == "/metrics") return path;
  if (path.rfind("/v1/", 0) == 0) return path;
  return "/";
}

int StatusCodeOf(const char *statusLine) { return std::atoi(statusLine); }

void RecordRequest(Deps &deps, const std::string &path, const char *statusLine) {
  deps.metrics->CounterAdd("sb_requests_total",
                            {{"endpoint", RouteLabel(path)}, {"status", std::to_string(StatusCodeOf(statusLine))}}, 1);
}

std::string OrAuto(const std::string &s) { return s.empty() ? "auto" : s; }

std::string Base64Encode(const std::string &data) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    uint32_t n = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) |
                 static_cast<uint8_t>(data[i + 2]);
    out += tbl[(n >> 18) & 0x3F];
    out += tbl[(n >> 12) & 0x3F];
    out += tbl[(n >> 6) & 0x3F];
    out += tbl[n & 0x3F];
  }
  size_t rem = data.size() - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint8_t>(data[i]) << 16;
    out += tbl[(n >> 18) & 0x3F];
    out += tbl[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t n = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
    out += tbl[(n >> 18) & 0x3F];
    out += tbl[(n >> 12) & 0x3F];
    out += tbl[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

struct AudioRequest {
  WavAudio audio;
  std::string source_lang, target_lang, voice;
};

// Mirrors readAudioRequest: multipart/form-data (file=<wav>, source_lang,
// target_lang, voice) or a raw audio/wav body with query params. `body` must
// have spare capacity for the multipart parser's padding trick.
AudioRequest ReadAudioRequest(std::string contentType, std::string &&body, const std::string &qSrc,
                               const std::string &qDst, const std::string &qVoice, int maxAudioSec) {
  AudioRequest out;
  std::string raw;
  if (contentType.rfind("multipart/form-data", 0) == 0) {
    ParsedMultipart mp;
    try {
      mp = ParseMultipart(contentType, body);
    } catch (const std::exception &) {
      throw WavError("bad multipart body");
    }
    out.source_lang = mp.fields.count("source_lang") ? mp.fields["source_lang"] : "";
    out.target_lang = mp.fields.count("target_lang") ? mp.fields["target_lang"] : "";
    out.voice = mp.fields.count("voice") ? mp.fields["voice"] : "";
    if (!mp.file) throw WavError("no audio file in request");
    raw = std::move(mp.file->data);
  } else {
    out.source_lang = qSrc;
    out.target_lang = qDst;
    out.voice = qVoice;
    raw = std::move(body);
  }

  out.audio = DecodeWav(raw);
  if (out.audio.duration_s > static_cast<double>(maxAudioSec)) {
    throw WavError("audio exceeds SB_MAX_AUDIO_SEC");
  }
  return out;
}

// Writes the appropriate error response for a ReadAudioRequest failure.
template <typename Res>
void AudioReqError(Res *res, const std::exception &e, const std::string &rid) {
  std::string msg = e.what();
  if (msg == "audio exceeds SB_MAX_AUDIO_SEC") {
    WriteError(res, kStatus413, "audio_too_long", msg, "", rid);
  } else if (dynamic_cast<const NotWavError *>(&e) || msg == "no audio file in request" ||
             msg == "bad multipart body") {
    WriteError(res, kStatus400, "bad_audio", msg, "", rid);
  } else {
    WriteError(res, kStatus400, "bad_request", msg, "", rid);
  }
}

// Reads the full request body (bounded by maxBytes, +1 spare byte for the
// multipart parser's in-place padding trick), then invokes `onComplete`.
// uWS requires everything captured from `req` to be copied out before this
// call returns, since req becomes invalid once the synchronous handler scope
// ends.
template <typename Res>
void ReadBody(Res *res, size_t maxBytes, std::function<void(std::string &&, bool tooLarge)> onComplete) {
  auto buf = std::make_shared<std::string>();
  auto aborted = std::make_shared<bool>(false);
  res->onAborted([aborted] { *aborted = true; });
  res->onData([res, buf, aborted, maxBytes, onComplete = std::move(onComplete)](std::string_view chunk,
                                                                                 bool last) mutable {
    if (*aborted) return;
    if (buf->size() + chunk.size() > maxBytes) {
      onComplete(std::string(), true);
      return;
    }
    buf->append(chunk.data(), chunk.size());
    if (last) {
      buf->reserve(buf->size() + 1);  // spare byte for multipart's padding trick
      onComplete(std::move(*buf), false);
    }
  });
}

}  // namespace

void RegisterHandlers(uWS::App &app, Deps &deps, Server &server, QueueGate &queue) {
  app.get("/health", [&deps](auto *res, auto * /*req*/) {
    std::string rid = obs::NewID();
    WriteJSON(res, kStatus200, rid, json{{"status", "ok"}});
    RecordRequest(deps, "/health", kStatus200);
  });

  app.get("/ready", [&deps](auto *res, auto * /*req*/) {
    std::string rid = obs::NewID();
    bool ready = true;
    json cores;
    for (auto &[name, st] : deps.core_state) {
      cores[name] = st;
      if (st != "ok" && st != "not configured") ready = false;
    }
    auto sttIt = deps.core_state.find("stt");
    if (sttIt == deps.core_state.end() || sttIt->second != "ok") ready = false;
    const char *status = ready ? kStatus200 : kStatus503;
    WriteJSON(res, status, rid, json{{"ready", ready}, {"cores", cores}});
    RecordRequest(deps, "/ready", status);
  });

  app.get("/metrics", [&deps](auto *res, auto * /*req*/) {
    std::string rid = obs::NewID();
    for (const char *name : {"stt", "mt", "tts_magpie", "tts_vits"}) {
      auto it = deps.core_state.find(name);
      double up = (it != deps.core_state.end() && it->second == "ok") ? 1.0 : 0.0;
      deps.metrics->GaugeSet("sb_core_up", {{"core", name}}, up);
    }
    res->writeStatus(kStatus200)
        ->writeHeader("Content-Type", "text/plain; version=0.0.4")
        ->writeHeader("X-Request-ID", rid)
        ->end(deps.metrics->Render());
    RecordRequest(deps, "/metrics", kStatus200);
  });

  app.get("/v1/capabilities", [&deps](auto *res, auto * /*req*/) {
    std::string rid = obs::NewID();
    auto caps = deps.pipeline->Capabilities();
    for (auto &[k, v] : caps) std::sort(v.begin(), v.end());
    json tts;
    for (auto &[k, v] : caps) tts[k] = v;
    json body = {{"cores", deps.core_state}, {"tts", tts}, {"stt_langs", {"uz", "ru", "kaa", "auto"}}};
    WriteJSON(res, kStatus200, rid, body);
    RecordRequest(deps, "/v1/capabilities", kStatus200);
  });

  // ---- batch endpoints (authed + queued) --------------------------------

  app.post("/v1/speech-to-speech", [&deps, &server, &queue](auto *res, auto *req) {
    std::string rid = obs::NewID();
    std::string authHeader(req->getHeader("authorization"));
    if (!server.CheckAuth(authHeader)) {
      WriteError(res, kStatus401, "unauthorized", "missing or invalid bearer token", "", rid);
      RecordRequest(deps, "/v1/speech-to-speech", kStatus401);
      return;
    }
    if (!queue.TryAcquire()) {
      WriteErrorRetryAfter(res, kStatus429, "queue_full", "server busy, retry shortly", rid, "2");
      RecordRequest(deps, "/v1/speech-to-speech", kStatus429);
      return;
    }
    std::string contentType(req->getHeader("content-type"));
    std::string qSrc(req->getQuery("source_lang"));
    std::string qDst(req->getQuery("target_lang"));
    std::string qVoice(req->getQuery("voice"));
    size_t maxBytes = static_cast<size_t>(deps.config->max_body_mb) << 20;

    ReadBody(res, maxBytes,
             [&deps, res, rid, contentType, qSrc, qDst, qVoice, &queue](std::string &&body, bool tooLarge) {
               auto done = [&](const char *status) {
                 queue.Release();
                 RecordRequest(deps, "/v1/speech-to-speech", status);
               };
               if (tooLarge) {
                 WriteError(res, kStatus413, "body_too_large", "request body exceeds SB_MAX_BODY_MB", "", rid);
                 return done(kStatus413);
               }
               AudioRequest ar;
               try {
                 ar = ReadAudioRequest(contentType, std::move(body), qSrc, qDst, qVoice, deps.config->max_audio_sec);
               } catch (const std::exception &e) {
                 AudioReqError(res, e, rid);
                 return done(kStatus400);
               }
               if (ar.target_lang.empty()) {
                 WriteError(res, kStatus400, "bad_request", "target_lang is required", "", rid);
                 return done(kStatus400);
               }

               pipeline::Pipeline::BatchInput in;
               in.audio = ar.audio.samples;
               in.sample_rate = ar.audio.sample_rate;
               in.source_lang = ar.source_lang;
               in.target_lang = ar.target_lang;
               in.voice = ar.voice;

               try {
                 auto out = deps.pipeline->Batch(in);
                 deps.metrics->Observe("stt", out.timings.stt_ms / 1000.0);
                 deps.metrics->Observe("mt", out.timings.mt_ms / 1000.0);
                 deps.metrics->Observe("tts", out.timings.tts_ms / 1000.0);
                 deps.metrics->Observe("total", out.timings.total_ms / 1000.0);
                 deps.metrics->CounterAdd("sb_audio_seconds_total", {}, ar.audio.duration_s);

                 deps.logger->Info("speech-to-speech",
                                    {{"stage", out.stage},
                                     {"duration_ms", out.timings.total_ms},
                                     {"stt_ms", out.timings.stt_ms},
                                     {"mt_ms", out.timings.mt_ms},
                                     {"tts_ms", out.timings.tts_ms},
                                     {"n_chars_transcript", out.transcript.size()}});
                 if (deps.logger->DebugEnabled()) {
                   deps.logger->Debug("speech-to-speech text",
                                       {{"transcript", out.transcript}, {"translation", out.translation}});
                 }

                 json body2 = {{"request_id", rid},
                                {"transcript", out.transcript},
                                {"translation", out.translation},
                                {"timings",
                                 {{"stt_ms", out.timings.stt_ms},
                                  {"mt_ms", out.timings.mt_ms},
                                  {"tts_ms", out.timings.tts_ms},
                                  {"total_ms", out.timings.total_ms}}},
                                {"stage", out.stage}};
                 if (!out.audio.empty()) {
                   body2["audio"] = Base64Encode(EncodeWav(out.audio, out.sample_rate));
                   body2["audio_format"] = "wav";
                   body2["sample_rate"] = out.sample_rate;
                 }
                 WriteJSON(res, kStatus200, rid, body2);
                 done(kStatus200);
               } catch (const pipeline::NoSTTError &) {
                 WriteError(res, kStatus503, "stt_unavailable", "speech-to-text engine not loaded", "stt", rid);
                 done(kStatus503);
               } catch (const std::exception &e) {
                 deps.logger->Error("speech-to-speech failed", {{"err", e.what()}});
                 WriteError(res, kStatus500, "pipeline_error", e.what(), "", rid);
                 done(kStatus500);
               }
             });
  });

  app.post("/v1/transcribe", [&deps, &server, &queue](auto *res, auto *req) {
    std::string rid = obs::NewID();
    std::string authHeader(req->getHeader("authorization"));
    if (!server.CheckAuth(authHeader)) {
      WriteError(res, kStatus401, "unauthorized", "missing or invalid bearer token", "", rid);
      RecordRequest(deps, "/v1/transcribe", kStatus401);
      return;
    }
    if (!queue.TryAcquire()) {
      WriteErrorRetryAfter(res, kStatus429, "queue_full", "server busy, retry shortly", rid, "2");
      RecordRequest(deps, "/v1/transcribe", kStatus429);
      return;
    }
    std::string contentType(req->getHeader("content-type"));
    std::string qSrc(req->getQuery("source_lang"));
    size_t maxBytes = static_cast<size_t>(deps.config->max_body_mb) << 20;

    ReadBody(res, maxBytes, [&deps, res, rid, contentType, qSrc, &queue](std::string &&body, bool tooLarge) {
      auto done = [&](const char *status) {
        queue.Release();
        RecordRequest(deps, "/v1/transcribe", status);
      };
      if (tooLarge) {
        WriteError(res, kStatus413, "body_too_large", "request body exceeds SB_MAX_BODY_MB", "", rid);
        return done(kStatus413);
      }
      AudioRequest ar;
      try {
        ar = ReadAudioRequest(contentType, std::move(body), qSrc, "", "", deps.config->max_audio_sec);
      } catch (const std::exception &e) {
        AudioReqError(res, e, rid);
        return done(kStatus400);
      }
      if (!deps.pipeline->HasSTT()) {
        WriteError(res, kStatus503, "stt_unavailable", "speech-to-text engine not loaded", "stt", rid);
        return done(kStatus503);
      }
      try {
        auto [transcript, sttMs] = deps.pipeline->Transcribe(ar.audio.samples, ar.audio.sample_rate, ar.source_lang);
        deps.metrics->Observe("stt", sttMs / 1000.0);
        deps.metrics->CounterAdd("sb_audio_seconds_total", {}, ar.audio.duration_s);
        WriteJSON(res, kStatus200, rid,
                  json{{"request_id", rid},
                       {"transcript", transcript},
                       {"timings", {{"stt_ms", sttMs}, {"total_ms", sttMs}}}});
        done(kStatus200);
      } catch (const std::exception &e) {
        WriteError(res, kStatus500, "stt_error", e.what(), "stt", rid);
        done(kStatus500);
      }
    });
  });

  app.post("/v1/translate", [&deps, &server, &queue](auto *res, auto *req) {
    std::string rid = obs::NewID();
    std::string authHeader(req->getHeader("authorization"));
    if (!server.CheckAuth(authHeader)) {
      WriteError(res, kStatus401, "unauthorized", "missing or invalid bearer token", "", rid);
      RecordRequest(deps, "/v1/translate", kStatus401);
      return;
    }
    if (!queue.TryAcquire()) {
      WriteErrorRetryAfter(res, kStatus429, "queue_full", "server busy, retry shortly", rid, "2");
      RecordRequest(deps, "/v1/translate", kStatus429);
      return;
    }
    size_t maxBytes = static_cast<size_t>(deps.config->max_body_mb) << 20;
    ReadBody(res, maxBytes, [&deps, res, rid, &queue](std::string &&body, bool tooLarge) {
      auto done = [&](const char *status) {
        queue.Release();
        RecordRequest(deps, "/v1/translate", status);
      };
      if (tooLarge) {
        WriteError(res, kStatus413, "body_too_large", "request body exceeds SB_MAX_BODY_MB", "", rid);
        return done(kStatus413);
      }
      json req_;
      std::string text, sourceLang, targetLang;
      try {
        req_ = json::parse(body);
        text = req_.value("text", "");
        sourceLang = req_.value("source_lang", "");
        targetLang = req_.value("target_lang", "");
      } catch (const std::exception &) {
        WriteError(res, kStatus400, "bad_request", "invalid JSON body", "", rid);
        return done(kStatus400);
      }
      if (text.empty() || targetLang.empty()) {
        WriteError(res, kStatus400, "bad_request", "text and target_lang are required", "", rid);
        return done(kStatus400);
      }
      if (!deps.pipeline->HasMT()) {
        WriteError(res, kStatus503, "mt_unavailable", "translation engine not loaded", "mt", rid);
        return done(kStatus503);
      }
      std::string translation;
      auto sentences = pipeline::Sentences(text);
      auto start = std::chrono::steady_clock::now();
      try {
        for (size_t i = 0; i < sentences.size(); i++) {
          auto out = deps.pipeline->Translate(sentences[i], OrAuto(sourceLang), targetLang);
          if (i) translation += " ";
          translation += pipeline::ForDisplay(out.text, targetLang);
        }
      } catch (const std::exception &e) {
        WriteError(res, kStatus500, "mt_error", e.what(), "mt", rid);
        return done(kStatus500);
      }
      deps.metrics->Observe(
          "mt", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      WriteJSON(res, kStatus200, rid, json{{"request_id", rid}, {"translation", translation}});
      done(kStatus200);
    });
  });

  app.post("/v1/speak", [&deps, &server, &queue](auto *res, auto *req) {
    std::string rid = obs::NewID();
    std::string authHeader(req->getHeader("authorization"));
    if (!server.CheckAuth(authHeader)) {
      WriteError(res, kStatus401, "unauthorized", "missing or invalid bearer token", "", rid);
      RecordRequest(deps, "/v1/speak", kStatus401);
      return;
    }
    if (!queue.TryAcquire()) {
      WriteErrorRetryAfter(res, kStatus429, "queue_full", "server busy, retry shortly", rid, "2");
      RecordRequest(deps, "/v1/speak", kStatus429);
      return;
    }
    size_t maxBytes = static_cast<size_t>(deps.config->max_body_mb) << 20;
    ReadBody(res, maxBytes, [&deps, res, rid, &queue](std::string &&body, bool tooLarge) {
      auto done = [&](const char *status) {
        queue.Release();
        RecordRequest(deps, "/v1/speak", status);
      };
      if (tooLarge) {
        WriteError(res, kStatus413, "body_too_large", "request body exceeds SB_MAX_BODY_MB", "", rid);
        return done(kStatus413);
      }
      json req_;
      std::string text, lang, voice, encoding;
      try {
        req_ = json::parse(body);
        text = req_.value("text", "");
        lang = req_.value("lang", "");
        voice = req_.value("voice", "");
        encoding = req_.value("encoding", "");
      } catch (const std::exception &) {
        WriteError(res, kStatus400, "bad_request", "invalid JSON body", "", rid);
        return done(kStatus400);
      }
      if (text.empty() || lang.empty()) {
        WriteError(res, kStatus400, "bad_request", "text and lang are required", "", rid);
        return done(kStatus400);
      }
      if (!deps.pipeline->TTSForLang(lang)) {
        WriteError(res, kStatus503, "tts_unavailable", "no TTS engine loaded for language " + lang, "tts", rid);
        return done(kStatus503);
      }
      std::vector<float> all;
      int sr = 0;
      auto start = std::chrono::steady_clock::now();
      for (auto &sent : pipeline::Sentences(text)) {
        pipeline::Pipeline::SynthResult sres;
        try {
          sres = deps.pipeline->Synth(sent, lang, voice);
        } catch (const std::exception &e) {
          WriteError(res, kStatus500, "tts_error", e.what(), "tts", rid);
          return done(kStatus500);
        }
        if (!sres.ok) {
          WriteError(res, kStatus503, "tts_unavailable", "no engine for " + lang, "tts", rid);
          return done(kStatus503);
        }
        if (sr == 0) sr = sres.sample_rate;
        all.insert(all.end(), sres.pcm.begin(), sres.pcm.end());
      }
      deps.metrics->Observe("tts", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

      std::string wav = EncodeWav(all, sr);
      if (encoding == "base64") {
        WriteJSON(res, kStatus200, rid,
                  json{{"request_id", rid}, {"audio", Base64Encode(wav)}, {"audio_format", "wav"}, {"sample_rate", sr}});
      } else {
        res->writeStatus(kStatus200)
            ->writeHeader("Content-Type", "audio/wav")
            ->writeHeader("X-Sample-Rate", std::to_string(sr))
            ->writeHeader("X-Request-ID", rid)
            ->end(wav);
      }
      done(kStatus200);
    });
  });
}

}  // namespace sb::httpapi
