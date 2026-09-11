#include "stream.h"

#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>

#include "../obs/log.h"
#include "errors.h"
#include "session.h"

namespace sb::httpapi {

namespace {

using json = nlohmann::json;

// Encodes float32 PCM as little-endian bytes for the binary WS frame,
// regardless of host endianness (this project only targets little-endian
// hosts today, but this costs nothing and matches stream.go's floatsToLE
// explicitly rather than assuming it).
std::string FloatsToLE(const std::vector<float> &f) {
  std::string b(f.size() * 4, '\0');
  for (size_t i = 0; i < f.size(); i++) {
    uint32_t u;
    std::memcpy(&u, &f[i], sizeof(u));
    b[i * 4 + 0] = static_cast<char>(u & 0xFF);
    b[i * 4 + 1] = static_cast<char>((u >> 8) & 0xFF);
    b[i * 4 + 2] = static_cast<char>((u >> 16) & 0xFF);
    b[i * 4 + 3] = static_cast<char>((u >> 24) & 0xFF);
  }
  return b;
}

struct WsUserData {
  std::shared_ptr<std::atomic<bool>> alive;
  std::shared_ptr<Session> session;
  std::string session_id;
  uWS::Loop *loop = nullptr;  // the real server loop, captured on upgrade — see upgrade's comment
};

using WS = uWS::WebSocket<false, true, WsUserData>;

}  // namespace

void RegisterStream(uWS::App &app, Deps &deps, Server &server, std::atomic<int64_t> &activeStreams) {
  int streamsMax = deps.config->streams_max > 0 ? deps.config->streams_max : 4;

  uWS::App::WebSocketBehavior<WsUserData> behavior;
  behavior.maxPayloadLength = 8 * 1024 * 1024;  // matches stream.go's c.SetReadLimit(8 << 20)
  behavior.idleTimeout = 60;                    // matches the mission's 60s idle timeout
  behavior.sendPingsAutomatically = true;

  behavior.upgrade = [&deps, &server, streamsMax, &activeStreams](auto *res, auto *req, auto *context) {
    if (activeStreams.load() >= streamsMax) {
      res->writeStatus(kStatus503)->end("too many streams");
      return;
    }
    std::string authHeader(req->getHeader("authorization"));
    std::string accessToken(req->getQuery("access_token"));
    if (!server.CheckAuthWS(authHeader, accessToken)) {
      res->writeStatus(kStatus401)->end("unauthorized");
      return;
    }
    std::string secKey(req->getHeader("sec-websocket-key"));
    std::string secProto(req->getHeader("sec-websocket-protocol"));
    std::string secExt(req->getHeader("sec-websocket-extensions"));

    WsUserData ud;
    ud.alive = std::make_shared<std::atomic<bool>>(true);
    ud.session_id = obs::NewID();
    // uWS::Loop::get() is thread-local: called from a worker thread it would
    // lazily create a brand-new loop nobody ever runs, silently swallowing
    // every defer() onto it. upgrade() runs on the real server loop thread,
    // so capture the actual loop here and hand it to every worker instead.
    ud.loop = uWS::Loop::get();
    res->template upgrade<WsUserData>(std::move(ud), secKey, secProto, secExt, context);
  };

  behavior.open = [&deps, &activeStreams](WS *ws) {
    activeStreams.fetch_add(1);
    deps.metrics->GaugeSet("sb_active_streams", {}, static_cast<double>(activeStreams.load()));
  };

  behavior.message = [&deps](WS *ws, std::string_view message, uWS::OpCode opCode) {
    auto *ud = ws->getUserData();

    if (opCode == uWS::OpCode::BINARY) {
      if (!ud->session) {
        ws->end(1002, "expected start frame");  // 1002 = protocol error
        return;
      }
      ud->session->PushAudio(reinterpret_cast<const uint8_t *>(message.data()), message.size());
      return;
    }

    // TEXT: JSON control frame.
    json j;
    try {
      j = json::parse(message);
    } catch (const std::exception &) {
      if (!ud->session) ws->end(1003, "invalid start frame");
      return;
    }
    std::string type = j.value("type", "");

    if (!ud->session) {
      if (type != "start") {
        ws->end(1002, "expected start frame");
        return;
      }
      Start s;
      s.type = "start";
      s.source_lang = j.value("source_lang", std::string());
      s.target_lang = j.value("target_lang", std::string());
      s.voice = j.value("voice", std::string());
      s.sample_rate = j.value("sample_rate", 16000);
      s.format = j.value("format", std::string("f32"));
      if (s.target_lang.empty()) {
        ws->end(1008, "target_lang required");  // 1008 = policy violation
        return;
      }

      auto alive = ud->alive;
      auto *loop = ud->loop;
      auto sendFn = [ws, alive, loop](Msg m) {
        loop->defer([ws, alive, m = std::move(m)]() mutable {
          if (!*alive) return;
          bool isDone = m.type == "done";
          nlohmann::ordered_json j = ToJSON(m);
          ws->send(j.dump(), uWS::OpCode::TEXT);
          if (m.type == "audio" && !m.binary.empty()) {
            ws->send(FloatsToLE(m.binary), uWS::OpCode::BINARY);
          }
          // Session::Stop() is non-blocking (see session.h) — the terminal
          // "done" message may arrive well after Stop() returns, once every
          // in-flight sentence worker finishes. Only close here, once it's
          // actually been sent, so no translation/audio message is dropped
          // (mirrors stream.go's blocking Stop() followed by c.Close()).
          if (isDone) ws->end(1000);
        });
      };

      try {
        ud->session = std::make_shared<Session>(deps.pipeline, s, sendFn);
      } catch (const std::exception &e) {
        nlohmann::ordered_json err;
        Msg m;
        m.type = "error";
        m.code = "stt_unavailable";
        m.stage = "stt";
        m.text = e.what();
        ws->send(ToJSON(m).dump(), uWS::OpCode::TEXT);
        ws->end(1011, "stt unavailable");  // 1011 = internal error
        return;
      }
      deps.logger->Info(
          "stream started",
          {{"session_id", ud->session_id}, {"source_lang", s.source_lang}, {"target_lang", s.target_lang}});
    } else if (type == "stop") {
      // Non-blocking; the connection closes once "done" is actually sent
      // (possibly asynchronously, after in-flight workers finish) — see sendFn.
      ud->session->Stop();
    }
    // any other frame type is silently ignored, matching stream.go's switch
  };

  behavior.close = [&deps, &activeStreams](WS *ws, int code, std::string_view msg) {
    auto *ud = ws->getUserData();
    deps.logger->Info("stream ws closed",
                       {{"session_id", ud->session_id}, {"code", code}, {"reason", std::string(msg)}});
    *ud->alive = false;
    if (ud->session) ud->session->Stop();  // non-blocking; safe even on abrupt disconnect
    activeStreams.fetch_sub(1);
    deps.metrics->GaugeSet("sb_active_streams", {}, static_cast<double>(activeStreams.load()));
  };

  app.ws<WsUserData>("/v1/stream", std::move(behavior));
}

}  // namespace sb::httpapi
